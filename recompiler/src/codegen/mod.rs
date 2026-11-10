//! turning the functions discovery found into C.
//!
//! every function becomes a C function that can be entered at any of its
//! labels. guest calls are C calls, and anything the code cannot follow on
//! its own, an svc, an unknown target, a full budget, returns all the way
//! out to the host, which picks up again from r15.

mod arm;
mod thumb;
mod vfp;

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write;

use crate::discover::{Analysis, Function, Mode, Program};
use crate::overrides::Override;

pub use recomp_abi::HEADER;

/// what the lowering of one instruction may refer to.
pub(crate) struct Scope<'a> {
    /// the labels of the function being written.
    pub labels: &'a BTreeSet<u32>,
    /// the C function that runs each function, by entry with bit 0 set
    /// for Thumb.
    pub functions: &'a BTreeMap<u32, String>,
    /// what the names of the functions start with, which keeps modules apart.
    pub prefix: &'a str,
    /// whether the code moves, so that addresses are offsets from where the
    /// host loaded it.
    pub relative: bool,
    /// the instructions left to the interpreter, which can end the code's
    /// run, so that it gets entered again right after them.
    pub interpreted: &'a std::cell::RefCell<Vec<u32>>,
}

impl Scope<'_> {
    /// an address as C.
    fn at(&self, address: u32) -> String {
        if self.relative { format!("(module_base + 0x{address:X}u)") } else { format!("0x{address:08X}u") }
    }

    /// the C function that runs the function at target, if there is one.
    fn function(&self, target: u32, thumb: bool) -> Option<String> {
        self.functions.get(&(target | thumb as u32)).cloned()
    }
}

fn name(prefix: &str, entry: u32, thumb: bool) -> String {
    format!("{prefix}{}_{entry:08X}", if thumb { 't' } else { 'f' })
}

macro_rules! emit {
    ($out:expr, $($arg:tt)*) => {
        writeln!($out, $($arg)*).unwrap()
    };
}

/// a call, with link the return address as lr holds it, bit 0 set when
/// the caller is Thumb.
fn call(out: &mut String, scope: &Scope, link: u32, target: u32, thumb: bool) -> bool {
    let caller_thumb = link & 1 != 0;
    let lr = if caller_thumb { format!("{} | 1", scope.at(link & !1)) } else { scope.at(link) };
    emit!(out, "    ctx->r[14] = {lr};");
    if thumb != caller_thumb {
        emit!(out, "    ctx->thumb = {};", thumb as u8);
    }
    match scope.function(target, thumb) {
        Some(name) => emit!(out, "    CALL_TO({name}, {}, {lr});", scope.at(target)),
        None => emit!(out, "    ctx->r[15] = {}; CALL_ELSEWHERE({lr});", scope.at(target)),
    }
    true
}

/// a branch without link, to a label of this function if it is one.
fn jump(out: &mut String, scope: &Scope, target: u32, thumb: bool) -> bool {
    if scope.labels.contains(&target) {
        emit!(out, "    goto L_{target:08X};");
    } else if let Some(name) = scope.function(target, thumb) {
        // a tail call
        emit!(out, "    TAIL_TO({name}, {});", scope.at(target));
    } else {
        emit!(out, "    target = {}; goto dispatch;", scope.at(target));
    }
    false
}

/// instructions per source file, so the files compile in parallel.
const FILE_SIZE: usize = 20_000;

/// whether a function becomes C, which it does unless its entry turned
/// out not to be code.
pub fn recompiles(entry: u32, function: &Function) -> bool {
    function.labels.contains(&entry)
}

/// an address as the host looks it up, bit 0 set for Thumb.
fn key(address: u32, mode: Mode) -> u32 {
    address | (mode == Mode::Thumb) as u32
}

/// the functions whose code another function already has, each with the
/// entry of the function that runs it. a jump table's cases and the target
/// of a tail call turn up both ways, as functions of their own and inside
/// the function that reaches them.
fn containers(functions: &BTreeMap<u32, &Function>) -> BTreeMap<u32, u32> {
    let mut owners: BTreeMap<u32, Vec<u32>> = BTreeMap::new();
    for (&entry, function) in functions {
        for &label in &function.labels {
            owners.entry(label).or_default().push(entry);
        }
    }
    // a function only moves into a bigger one, so the links cannot loop
    let rank = |entry: u32| (functions[&entry].instructions.len(), std::cmp::Reverse(entry));
    let mut parent = BTreeMap::new();
    for (&entry, function) in functions {
        let contains = |other: u32| {
            let code = &functions[&other].instructions;
            function.instructions.iter().all(|address| code.binary_search(address).is_ok())
        };
        let best = owners[&entry]
            .iter()
            .copied()
            .filter(|&other| functions[&other].mode == function.mode && rank(other) > rank(entry))
            .filter(|&other| contains(other))
            .max_by_key(|&other| rank(other));
        if let Some(best) = best {
            parent.insert(entry, best);
        }
    }
    parent
        .keys()
        .map(|&entry| {
            let mut container = parent[&entry];
            while let Some(&next) = parent.get(&container) {
                container = next;
            }
            (entry, container)
        })
        .collect()
}

/// a program to write as C, the executable or one of its modules.
pub struct Unit<'a> {
    /// the module's name, None for the executable, whose code does not move.
    pub module: Option<&'a str>,
    pub program: &'a Program,
    pub analysis: &'a Analysis,
}

/// the C sources for the units, the executable first, as file names and
/// contents. the overrides take the place of the functions they replace,
/// which overrides.h lets them still call.
pub fn generate(units: &[Unit], overrides: &[Override]) -> Vec<(String, String)> {
    let mut files = vec![("recomp.h".to_owned(), HEADER.to_owned())];
    let mut sources: Vec<String> = Vec::new();
    let mut source = String::new();
    // the headers the file being written includes
    let mut included = BTreeSet::new();
    let mut size = 0;
    // RECOMP_TABLES: this file has recomp_enter, which sets the registers
    // pinned code keeps to itself, so it must not claim them
    let mut tables = String::from("#define RECOMP_TABLES\n#include \"recomp.h\"\n");
    let mut modules = String::new();
    let mut originals = String::new();
    let mut original_headers = BTreeSet::new();

    for (index, unit) in units.iter().enumerate() {
        let prefix = if unit.module.is_some() { format!("m{index:03}_") } else { String::new() };
        let header = format!("{}functions.h", prefix);
        let mut functions: BTreeMap<u32, &Function> =
            unit.analysis.functions.iter().filter(|&(&entry, f)| recompiles(entry, f)).map(|(&e, f)| (e, f)).collect();
        let containers = containers(&functions);
        let mut names: BTreeMap<u32, String> = functions
            .iter()
            .map(|(&entry, f)| {
                let home = containers.get(&entry).copied().unwrap_or(entry);
                (key(entry, f.mode), name(&prefix, home, f.mode == Mode::Thumb))
            })
            .collect();
        functions.retain(|entry, _| !containers.contains_key(entry));

        // calls go to the functions written by hand, which reach the
        // generated ones through a function that enters them where they
        // replace them
        let replaced: Vec<&Override> = overrides.iter().filter(|o| o.module.as_deref() == unit.module).collect();
        for &item in &replaced {
            let Some(original) = names.insert(item.address, item.name.clone()) else { continue };
            let address = item.address & !1;
            let at = if unit.module.is_some() { format!("{prefix}base + 0x{address:X}u") } else { format!("0x{address:08X}u") };
            emit!(originals, "static inline void {}(Context *ctx) {{", item.original);
            emit!(originals, "    ctx->r[15] = {at};");
            emit!(originals, "    ctx->thumb = {};", item.address & 1);
            emit!(originals, "    {original}(ctx);");
            emit!(originals, "}}\n");
            original_headers.insert(header.clone());
        }

        let mut prototypes = String::new();
        if unit.module.is_some() {
            emit!(prototypes, "extern uint32_t {prefix}base;");
        }
        for item in &replaced {
            emit!(prototypes, "RECOMP_FN({});", item.name);
        }
        for (&entry, function) in &functions {
            emit!(prototypes, "RECOMP_FN({});", name(&prefix, entry, function.mode == Mode::Thumb));
        }
        files.push((header.clone(), prototypes));

        let include = format!("#include \"{header}\"\n\n");
        // each function's labels with those after what it interprets
        let mut resumed: BTreeMap<u32, BTreeSet<u32>> = BTreeMap::new();
        for (&entry, function) in &functions {
            if source.is_empty() {
                source.push_str("#include \"recomp.h\"\n#if defined(RECOMP_FASTMEM) && !defined(RECOMP_PINNED)\n#undef RECOMP_VIEW\n#define RECOMP_VIEW(ctx) view\n#endif\n");
            }
            if included.insert(index) {
                source.push_str(&include);
            }
            // written once to see what goes to the interpreter, the code
            // can stop after any of those, and then again with a label
            // after each, where the host comes back in
            let interpreted = std::cell::RefCell::new(Vec::new());
            let relative = unit.module.is_some();
            let scope = Scope { labels: &function.labels, functions: &names, prefix: &prefix, relative, interpreted: &interpreted };
            write_function(&mut String::new(), unit.program, &scope, entry, function);
            let mut resumable = (*function).clone();
            for address in interpreted.take() {
                let at = function.instructions.iter().position(|&a| a == address);
                if let Some(&next) = at.and_then(|i| function.instructions.get(i + 1)) {
                    resumable.labels.insert(next);
                }
            }
            let scope = Scope { labels: &resumable.labels, functions: &names, prefix: &prefix, relative, interpreted: &interpreted };
            write_function(&mut source, unit.program, &scope, entry, &resumable);
            resumed.insert(entry, resumable.labels);
            size += function.instructions.len();
            if size >= FILE_SIZE {
                sources.push(std::mem::take(&mut source));
                included.clear();
                size = 0;
            }
        }

        // every label the host can resume at, preferring the function that
        // starts there
        let mut entries: BTreeMap<u32, String> = BTreeMap::new();
        for (&entry, function) in &functions {
            let owner = name(&prefix, entry, function.mode == Mode::Thumb);
            for &label in resumed.get(&entry).unwrap_or(&function.labels) {
                let slot = entries.entry(key(label, function.mode)).or_insert_with(|| owner.clone());
                if label == entry {
                    slot.clone_from(&owner);
                }
            }
        }
        for item in &replaced {
            entries.insert(item.address, item.name.clone());
        }
        tables.push_str(&include);
        let table = match unit.module {
            Some(module) => {
                emit!(tables, "uint32_t {prefix}base;");
                emit!(
                    modules,
                    "    {{\"{module}\", &{prefix}base, 0x{:X}u, {}, {prefix}entries}},",
                    unit.program.text.end(),
                    entries.len()
                );
                format!("static const Entry {prefix}entries[]")
            }
            None => {
                emit!(tables, "RECOMP_EXPORT const uint32_t recomp_entry_count = {};", entries.len());
                "RECOMP_EXPORT const Entry recomp_entries[]".to_owned()
            }
        };
        emit!(tables, "{table} = {{");
        for (label, owner) in entries {
            emit!(tables, "    {{0x{label:08X}u, {owner}}},");
        }
        emit!(tables, "}};\n");
    }
    if !source.is_empty() {
        sources.push(source);
    }

    emit!(tables, "RECOMP_EXPORT const uint32_t recomp_abi = RECOMP_ABI;");
    // code built to reach memory straight through the host's view of it
    emit!(tables, "#ifdef RECOMP_FASTMEM\nRECOMP_EXPORT const uint32_t recomp_fastmem = 1;\n#endif");
    emit!(tables, "RECOMP_EXPORT const uint32_t recomp_module_count = {};", units.len() - 1);
    emit!(tables, "RECOMP_EXPORT const Module recomp_modules[] = {{\n{modules}}};");
    tables.push_str(ENTER);
    files.push(("entries.c".to_owned(), tables));
    if !overrides.is_empty() {
        let mut header = String::from("/* what overrides include, see docs/overrides.md. */\n\n#include \"recomp.h\"\n");
        for included in &original_headers {
            emit!(header, "#include \"{included}\"");
        }
        emit!(header, "\n/* the generated functions the overrides replace. */\n\n{originals}");
        files.push(("overrides.h".to_owned(), header));
    }
    files.extend(sources.into_iter().enumerate().map(|(i, source)| (format!("code{i:03}.c"), source)));
    files
}

/// recomp_enter, which a host runs pinned code through: it keeps the host's
/// registers the code takes for the guest's, loads the guest's from the
/// context, runs the code from r15 and stores them back, with r15 and the
/// instruction set of where the code returned to when it did not leave by
/// itself. The stack floor is a megabyte under where the code starts, far
/// more guest calls than the 2048 unpinned code counts to, far less than a
/// thread's stack.
const ENTER: &str = r#"
#ifdef RECOMP_PINNED
#include <stddef.h>
RECOMP_EXPORT void recomp_enter(Context *context, Code code) {
    register Context *c __asm__("x0") = context;
    register Code f __asm__("x1") = code;
    __asm__ volatile(
        "mov x19, x0\n"
        "ldr x20, [x19, %[fastmem]]\n"
        "ldr w21, [x19, %[budget]]\n"
        "mov x2, sp\n"
        "sub x2, x2, #0x100, lsl #12\n"
        "str x2, [x19, %[floor]]\n"
        "ldp w22, w23, [x19, #0]\n"
        "ldp w24, w25, [x19, #8]\n"
        "ldp w26, w27, [x19, #16]\n"
        "ldp w28, w9, [x19, #24]\n"
        "ldp w10, w11, [x19, #32]\n"
        "ldp w12, w13, [x19, #40]\n"
        "ldp w14, w15, [x19, #48]\n"
        "ldr w18, [x19, #56]\n"
        "mov w0, #1\n"
        "blr x1\n"
        "ldr w2, [x19, %[exit]]\n"
        "cbnz w2, 1f\n"
        "and w2, w0, #1\n"
        "strb w2, [x19, %[thumb]]\n"
        "mov w3, #-4\n"
        "mov w4, #-2\n"
        "cmp w2, #0\n"
        "csel w3, w4, w3, ne\n"
        "and w0, w0, w3\n"
        "str w0, [x19, #60]\n"
        "1:\n"
        "stp w22, w23, [x19, #0]\n"
        "stp w24, w25, [x19, #8]\n"
        "stp w26, w27, [x19, #16]\n"
        "stp w28, w9, [x19, #24]\n"
        "stp w10, w11, [x19, #32]\n"
        "stp w12, w13, [x19, #40]\n"
        "stp w14, w15, [x19, #48]\n"
        "str w18, [x19, #56]\n"
        "str w21, [x19, %[budget]]\n"
        : "+r"(c), "+r"(f)
        : [fastmem] "i"(offsetof(Context, fastmem)), [budget] "i"(offsetof(Context, budget)),
          [floor] "i"(offsetof(Context, stack_floor)), [exit] "i"(offsetof(Context, exit)),
          [thumb] "i"(offsetof(Context, thumb))
        : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
          "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory");
}
#endif
"#;

/// the body with r0 to r14 named by the R macros, which are the context's
/// registers, or the host's own in pinned code. r15 stays the context's.
fn registers_by_name(body: &str) -> String {
    let mut out = String::with_capacity(body.len());
    let mut rest = body;
    while let Some(at) = rest.find("ctx->r[") {
        out.push_str(&rest[..at]);
        let after = &rest[at + 7..];
        let digits = after.bytes().take_while(u8::is_ascii_digit).count();
        match after[..digits].parse::<u32>() {
            Ok(register) if register < 15 && after[digits..].starts_with(']') => {
                write!(out, "R{register}").unwrap();
                rest = &after[digits + 1..];
            }
            _ => {
                out.push_str("ctx->r[");
                rest = after;
            }
        }
    }
    out.push_str(rest);
    out
}

fn write_function(out: &mut String, program: &Program, scope: &Scope, entry: u32, function: &Function) {
    let mut body = String::new();
    write_body(&mut body, program, scope, entry, function);
    let body = registers_by_name(&body);
    // the function's instruction set, which a return compares where it
    // goes with
    emit!(out, "#undef RECOMP_THUMB\n#define RECOMP_THUMB {}", (function.mode == Mode::Thumb) as u8);
    let head = format!("RECOMP_FN({}) {{\n    RECOMP_HEAD\n", name(scope.prefix, entry, function.mode == Mode::Thumb));
    match flags_in_locals(&body) {
        Some(body) => {
            out.push_str("#undef RECOMP_SYNC_OUT\n#undef RECOMP_SYNC_IN\n");
            out.push_str("#define RECOMP_SYNC_OUT() RECOMP_FLAGS_OUT()\n#define RECOMP_SYNC_IN() RECOMP_FLAGS_IN()\n");
            out.push_str(&head);
            out.push_str("    RECOMP_FLAGS_DECL\n");
            out.push_str(&body);
            out.push_str("#undef RECOMP_SYNC_OUT\n#undef RECOMP_SYNC_IN\n");
            out.push_str("#define RECOMP_SYNC_OUT() do {} while (0)\n#define RECOMP_SYNC_IN() do {} while (0)\n");
        }
        None => {
            out.push_str(&head);
            out.push_str(&body);
        }
    }
}

/// a function's body with the flags named by the F_ macros, which built with
/// RECOMP_FLAG_LOCALS are locals the compiler keeps in registers and drops
/// when nothing reads them before they are set again, stored to the context
/// only where code outside the function can see them. None when the function
/// never touches them.
fn flags_in_locals(body: &str) -> Option<String> {
    const CONDITIONS: [&str; 14] = ["C_EQ", "C_NE", "C_CS", "C_CC", "C_MI", "C_PL", "C_VS", "C_VC", "C_HI", "C_LS", "C_GE", "C_LT", "C_GT", "C_LE"];
    let mut out = String::with_capacity(body.len() + body.len() / 8);
    let mut used = CONDITIONS.iter().any(|condition| body.contains(condition));
    let mut rest = body;
    while let Some(at) = rest.find("ctx->") {
        out.push_str(&rest[..at]);
        let after = &rest[at + 5..];
        let mut chars = after.chars();
        match (chars.next(), chars.next()) {
            (Some(flag @ ('n' | 'z' | 'c' | 'v')), next) if !next.is_some_and(|c| c.is_ascii_alphanumeric() || c == '_') => {
                out.push_str("F_");
                out.push(flag.to_ascii_uppercase());
                rest = &after[1..];
                used = true;
            }
            _ => {
                out.push_str("ctx->");
                rest = after;
            }
        }
    }
    out.push_str(rest);
    used.then(|| out.replace("return;", "{ RECOMP_SYNC_OUT(); return; }"))
}

fn write_body(out: &mut String, program: &Program, scope: &Scope, entry: u32, function: &Function) {
    let thumb = function.mode == Mode::Thumb;
    let state = if thumb { "ctx->thumb" } else { "!ctx->thumb" };
    if scope.relative {
        emit!(out, "    const uint32_t module_base = {}base;", scope.prefix);
    }
    // the instructions first, to know whether the function calls others
    let mut code = String::new();
    let instructions = &function.instructions;
    for (i, &address) in instructions.iter().enumerate() {
        if function.labels.contains(&address) {
            let run = instructions[i + 1..].iter().take_while(|a| !function.labels.contains(a)).count() + 1;
            emit!(code, "L_{address:08X}:");
            emit!(code, "    BUDGET({}, {run});", scope.at(address));
        }
        let (continues, size) = if thumb {
            let op = program.text.read16(address).unwrap_or(0) as u32;
            let second = program.text.read16(address + 2).unwrap_or(0) as u32;
            let size = crate::thumb::decode(op as u16, Some(second as u16), address).1;
            emit!(code, "    /* {address:08X} {op:04X} */");
            (thumb::lower(&mut code, scope, address, op, second), size)
        } else {
            let op = program.text.read32(address).unwrap_or(0);
            emit!(code, "    /* {address:08X} {op:08X} */");
            (arm::lower(&mut code, scope, address, op), 4)
        };
        let next = address + size;
        if continues && instructions.get(i + 1) != Some(&next) {
            emit!(code, "    target = {}; goto dispatch;", scope.at(next));
        }
    }
    let calls = ["CALL_TO(", "CALL_ELSEWHERE(", "TAIL_TO("].iter().any(|call| code.contains(call));
    let enter = if calls { "RECOMP_ENTRY_CALLS" } else { "RECOMP_ENTRY" };
    emit!(out, "    {enter}({}, L_{entry:08X}, {state})", scope.at(entry));
    emit!(out, "dispatch:");
    let offset = if scope.relative { "target - module_base" } else { "target" };
    emit!(out, "    if ({state}) switch ({offset}) {{");
    for label in &function.labels {
        emit!(out, "    case 0x{label:08X}u: goto L_{label:08X};");
    }
    emit!(out, "    }}");
    // somewhere else, the host knows where
    emit!(out, "    TAIL_ELSEWHERE(target);");
    out.push_str(&code);
    emit!(out, "}}\n");
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::discover::{self, Source};
    use crate::image::Segment;

    const BASE: u32 = 0x0010_0000;

    /// the files for a function that calls another, the callee replaced.
    fn generated(overrides: &[Override]) -> BTreeMap<String, String> {
        // bl BASE + 8, bx lr, bx lr
        let words = [0xEB00_0000u32, 0xE12F_FF1E, 0xE12F_FF1E];
        let program = Program {
            text: Segment { base: BASE, bytes: words.iter().flat_map(|w| w.to_le_bytes()).collect() },
            seeds: vec![(BASE, Source::Entry)],
            slots: None,
        };
        let analysis = discover::analyze(&program);
        let units = [Unit { module: None, program: &program, analysis: &analysis }];
        generate(&units, overrides).into_iter().collect()
    }

    #[test]
    fn overrides_take_the_place_of_what_they_replace() {
        let replaced = Override {
            module: None,
            address: BASE + 8,
            name: "override_0x00100008".to_owned(),
            original: "original_0x00100008".to_owned(),
        };
        let files = generated(std::slice::from_ref(&replaced));
        let code = &files["code000.c"];
        assert!(code.contains("CALL_TO(override_0x00100008, 0x00100008u, 0x00100004u);"));
        assert!(files["functions.h"].contains("RECOMP_FN(override_0x00100008);"));
        assert!(files["entries.c"].contains("{0x00100008u, override_0x00100008},"));
        let header = &files["overrides.h"];
        assert!(header.contains("static inline void original_0x00100008(Context *ctx) {"));
        assert!(header.contains("f_00100008(ctx);"));

        // without overrides nothing changes
        let plain = generated(&[]);
        assert!(plain["code000.c"].contains("CALL_TO(f_00100008, 0x00100008u, 0x00100004u);"));
        assert!(!plain.contains_key("overrides.h"));
    }
}
