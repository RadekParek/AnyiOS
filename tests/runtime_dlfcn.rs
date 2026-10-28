// Host-stub harness that compiles the real dlfcn module without SDL or a guest
// process. Memory, CPU, host library exports and trampoline creation are mocks;
// this validates path/handle/scope bookkeeping, not real dynamic linking or ABI.
#![allow(dead_code, non_camel_case_types, unused_imports, unused_variables)]

#[macro_export]
macro_rules! export_c_func {
    ($($t:tt)*) => {
        ()
    };
}
#[macro_export]
macro_rules! log_dbg {
    ($($t:tt)*) => {{}};
}

pub type ThreadId = usize;

mod mem {
    use std::marker::PhantomData;

    pub type GuestUSize = u32;
    #[repr(transparent)]
    pub struct Ptr<T, const MUT: bool>(u32, PhantomData<T>);
    impl<T, const MUT: bool> Copy for Ptr<T, MUT> {}
    impl<T, const MUT: bool> Clone for Ptr<T, MUT> {
        fn clone(&self) -> Self {
            *self
        }
    }
    impl<T, const MUT: bool> std::fmt::Debug for Ptr<T, MUT> {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            write!(f, "{:#x}", self.0)
        }
    }
    impl<T, const MUT: bool> PartialEq for Ptr<T, MUT> {
        fn eq(&self, rhs: &Self) -> bool {
            self.0 == rhs.0
        }
    }
    impl<T, const MUT: bool> Eq for Ptr<T, MUT> {}
    impl<T, const MUT: bool> Ptr<T, MUT> {
        pub const fn from_bits(bits: u32) -> Self {
            Self(bits, PhantomData)
        }
        pub const fn null() -> Self {
            Self::from_bits(0)
        }
        pub fn to_bits(self) -> u32 {
            self.0
        }
        pub fn is_null(self) -> bool {
            self.0 == 0
        }
        pub fn cast<U>(self) -> Ptr<U, MUT> {
            Ptr::from_bits(self.0)
        }
    }
    impl<T> Ptr<T, true> {
        pub fn cast_const(self) -> Ptr<T, false> {
            Ptr::from_bits(self.0)
        }
    }
    pub type ConstPtr<T> = Ptr<T, false>;
    pub type MutPtr<T> = Ptr<T, true>;
    pub type ConstVoidPtr = ConstPtr<std::ffi::c_void>;
    pub type MutVoidPtr = MutPtr<std::ffi::c_void>;
    pub unsafe trait SafeRead: Copy {}

    pub struct Mem {
        data: Vec<u8>,
        next: u32,
    }
    impl Mem {
        pub fn new() -> Self {
            Self {
                data: vec![0; 1024 * 1024],
                next: 0x1000,
            }
        }
        pub fn alloc(&mut self, n: u32) -> MutVoidPtr {
            let ptr = self.next;
            self.next += (n + 3) & !3;
            assert!((self.next as usize) < self.data.len());
            Ptr::from_bits(ptr)
        }
        pub fn alloc_and_write_cstr(&mut self, value: &[u8]) -> MutPtr<u8> {
            let ptr: MutPtr<u8> = self.alloc(value.len() as u32 + 1).cast();
            let dest = self.bytes_at_mut(ptr, value.len() as u32 + 1);
            dest[..value.len()].copy_from_slice(value);
            dest[value.len()] = 0;
            ptr
        }
        pub fn free(&mut self, _ptr: MutVoidPtr) {}
        pub fn bytes_at_mut(&mut self, ptr: MutPtr<u8>, n: u32) -> &mut [u8] {
            &mut self.data[ptr.to_bits() as usize..(ptr.to_bits() + n) as usize]
        }
        pub fn cstr_at_utf8(&self, ptr: ConstPtr<u8>) -> Result<&str, ()> {
            let data = self.data.get(ptr.to_bits() as usize..).ok_or(())?;
            let end = data.iter().position(|&byte| byte == 0).ok_or(())?;
            std::str::from_utf8(&data[..end]).map_err(|_| ())
        }
        pub fn write<T: SafeRead>(&mut self, _ptr: MutPtr<T>, _value: T) {}
    }
}

mod mach_o {
    use std::collections::HashMap;
    pub enum SectionType {
        Normal,
    }
    pub struct Section {
        pub name: String,
        pub addr: u32,
        pub size: u32,
        pub type_: SectionType,
        pub dyld_indirect_symbol_info: Option<()>,
    }
    pub struct MachO {
        pub name: String,
        pub dynamic_libraries: Vec<String>,
        pub sections: Vec<Section>,
        pub exported_symbols: HashMap<String, u32>,
        pub external_symbols: HashMap<String, u32>,
        pub external_relocations: Vec<(u32, String)>,
        pub entry_point_pc: Option<u32>,
        pub entry_point_is_lc_main: bool,
        pub last_segment_end: u32,
        pub text_base: u32,
        pub vmaddr_slide: u32,
    }
}

mod environment {
    // Matches the known bundled aliases in the real environment loader.
    pub fn canonicalize_dylib_path(path: &str) -> String {
        match path {
            "libz.1.dylib" | "libz.dylib" | "libz.1.1.3.dylib" => "libz.1.2.3.dylib",
            "libstdc++.6.dylib" => "libstdc++.6.0.9.dylib",
            "libsqlite3.0.dylib" => "libsqlite3.dylib",
            _ => path,
        }
        .to_owned()
    }
}

mod cpu {
    #[derive(Default)]
    pub struct Cpu {
        regs: [u32; 16],
    }
    impl Cpu {
        pub const LR: usize = 14;
        pub fn regs(&self) -> &[u32; 16] {
            &self.regs
        }
    }
}

mod dyld {
    pub use crate::export_c_func;
    pub type FunctionExports = &'static [()];
    pub struct HostDylib {
        pub path: &'static str,
        pub aliases: &'static [&'static str],
        pub function_exports: &'static [&'static [(&'static str, ())]],
    }
    static SQLITE: HostDylib = HostDylib {
        path: "/usr/lib/libsqlite3.dylib",
        aliases: &["/usr/lib/libsqlite3.0.dylib"],
        function_exports: &[&[("_sqlite3_open", ())]],
    };
    pub static DYLIB_LIST: &[&HostDylib] = &[&SQLITE];
    pub struct GuestFunction(u32);
    impl GuestFunction {
        pub fn addr_with_thumb_bit(self) -> u32 {
            self.0
        }
    }
    #[derive(Default)]
    pub struct Dyld;
    impl Dyld {
        pub fn dlsym_abi_intercept(
            &mut self,
            _mem: &mut crate::mem::Mem,
            _cpu: &mut crate::cpu::Cpu,
            symbol: &str,
        ) -> Option<GuestFunction> {
            (symbol == "___dynamic_cast").then_some(GuestFunction(0x8888))
        }
        pub fn create_scoped_host_proc_address(
            &mut self,
            _mem: &mut crate::mem::Mem,
            _cpu: &mut crate::cpu::Cpu,
            index: usize,
            symbol: &str,
        ) -> Result<GuestFunction, ()> {
            if index == 0 && symbol == "_sqlite3_open" {
                Ok(GuestFunction(0x7000))
            } else {
                Err(())
            }
        }
    }
}

#[path = "../third_party/anyiose-engine/src/libc/dlfcn.rs"]
mod dlfcn;

#[derive(Default)]
struct LibcState {
    dlfcn: dlfcn::State,
}
struct GuestPath(String);
impl GuestPath {
    fn as_str(&self) -> &str {
        &self.0
    }
}
struct Bundle;
impl Bundle {
    fn executable_path(&self) -> GuestPath {
        GuestPath("/App".to_owned())
    }
}
struct Environment {
    mem: mem::Mem,
    bins: Vec<mach_o::MachO>,
    cpu: cpu::Cpu,
    dyld: dyld::Dyld,
    libc_state: LibcState,
    bundle: Bundle,
    current_thread: ThreadId,
}

#[cfg(test)]
mod integration_tests {
    use super::*;
    use mem::{ConstPtr, MutVoidPtr, Ptr};
    use std::collections::HashMap;

    fn image(name: &str, base: u32) -> mach_o::MachO {
        mach_o::MachO {
            name: name.to_owned(),
            dynamic_libraries: vec![],
            sections: vec![],
            exported_symbols: HashMap::new(),
            external_symbols: HashMap::new(),
            external_relocations: vec![],
            entry_point_pc: None,
            entry_point_is_lc_main: false,
            last_segment_end: base + 0x4000,
            text_base: base,
            vmaddr_slide: 0,
        }
    }

    fn env() -> Environment {
        let mut app = image("app", 0x1000);
        app.external_symbols.insert("_shared".into(), 0x2011);
        app.exported_symbols.insert("_private".into(), 0x2020);
        app.exported_symbols
            .insert("___dynamic_cast".into(), 0x2030);
        let mut first = image("libfirst.dylib", 0x6000);
        first.external_symbols.insert("_shared".into(), 0x7011);
        first
            .dynamic_libraries
            .push("/usr/lib/libsecond.dylib".into());
        first
            .dynamic_libraries
            .push("/usr/lib/libsqlite3.dylib".into());
        let mut second = image("libsecond.dylib", 0xb000);
        second.external_symbols.insert("_dependency".into(), 0xc011);
        Environment {
            mem: mem::Mem::new(),
            bins: vec![app, first, second],
            cpu: cpu::Cpu::default(),
            dyld: dyld::Dyld,
            libc_state: LibcState::default(),
            bundle: Bundle,
            current_thread: 0,
        }
    }

    fn text(env: &mut Environment, s: &str) -> ConstPtr<u8> {
        env.mem.alloc_and_write_cstr(s.as_bytes()).cast_const()
    }
    fn symbol(env: &mut Environment, handle: MutVoidPtr, s: &str) -> u32 {
        let name = text(env, s);
        dlfcn::dlsym(env, handle, name).to_bits()
    }
    fn take_error(env: &mut Environment) -> Option<String> {
        let value = dlfcn::dlerror(env);
        if value.is_null() {
            None
        } else {
            Some(env.mem.cstr_at_utf8(value).unwrap().to_owned())
        }
    }

    #[test]
    fn actual_dlopen_dlsym_and_dlclose_honor_scopes_and_references() {
        let mut env = env();
        let path = text(&mut env, "/usr/lib/libfirst.dylib");
        let a = dlfcn::dlopen(&mut env, path, 0);
        let b = dlfcn::dlopen(&mut env, path, 0);
        let first = dlfcn::dlopen(&mut env, path, 0x100);
        assert_eq!(a, b);
        assert_ne!(a, first);
        assert_eq!(symbol(&mut env, a, "shared"), 0x7011); // not main's export
        assert_eq!(symbol(&mut env, a, "dependency"), 0xc011);
        assert_eq!(symbol(&mut env, a, "sqlite3_open"), 0x7000); // explicit host dependency
        assert_eq!(symbol(&mut env, first, "dependency"), 0);
        assert!(take_error(&mut env).unwrap().contains("symbol not found"));
        assert_eq!(symbol(&mut env, first, "sqlite3_open"), 0);
        assert!(take_error(&mut env).is_some());
        assert_eq!(
            symbol(&mut env, Ptr::from_bits(-5i32 as u32), "shared"),
            0x2011
        );
        assert_eq!(symbol(&mut env, Ptr::from_bits(-5i32 as u32), "private"), 0);
        assert_eq!(
            symbol(&mut env, Ptr::from_bits(-5i32 as u32), "__dynamic_cast"),
            0
        ); // private is not visible
        env.bins[0]
            .external_symbols
            .insert("___dynamic_cast".into(), 0x2040);
        assert_eq!(
            symbol(&mut env, Ptr::from_bits(-5i32 as u32), "__dynamic_cast"),
            0x8888
        ); // visible ABI intercept

        assert!(take_error(&mut env).is_some());
        assert_eq!(dlfcn::dlclose(&mut env, a), 0);
        assert_eq!(symbol(&mut env, b, "shared"), 0x7011);
        assert_eq!(dlfcn::dlclose(&mut env, b), 0);
        assert_eq!(symbol(&mut env, a, "shared"), 0);
        assert!(take_error(&mut env).unwrap().contains("invalid handle"));
        assert_eq!(dlfcn::dlclose(&mut env, a), -1);
    }

    #[test]
    fn actual_host_handles_unknown_paths_and_errors_are_scoped() {
        let mut env = env();
        let sqlite = text(&mut env, "/usr/lib/libsqlite3.dylib");
        let handle = dlfcn::dlopen(&mut env, sqlite, 0x100);
        assert_eq!(symbol(&mut env, handle, "sqlite3_open"), 0x7000);
        assert_eq!(symbol(&mut env, handle, "shared"), 0); // do not leak guest exports
        env.current_thread = 1;
        assert_eq!(take_error(&mut env), None);
        let unknown = text(&mut env, "/unknown/libsqlite3.dylib");
        assert!(dlfcn::dlopen(&mut env, unknown, 0).is_null());
        assert!(take_error(&mut env).unwrap().contains("image not found"));
        assert_eq!(take_error(&mut env), None);
        env.current_thread = 0;
        assert!(take_error(&mut env).unwrap().contains("symbol not found"));
        assert_eq!(take_error(&mut env), None);
        let default = dlfcn::dlopen(&mut env, Ptr::null(), 0);
        assert_eq!(symbol(&mut env, default, "shared"), 0x2011);
        assert_eq!(dlfcn::dlclose(&mut env, default), 0);
    }
}
