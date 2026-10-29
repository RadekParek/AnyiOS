// Exercise the pinned-engine UIEdgeInsets implementation without SDL or a
// device. The guest-memory/ABI scaffolding below is a host stand-in: the
// TestApp guest tests and full Cargo tests remain the integration checks.
#![allow(dead_code)]

mod frameworks {
    pub mod core_graphics {
        pub type CGFloat = f32;
    }
}

mod mem {
    use std::marker::PhantomData;
    pub type GuestUSize = u32;
    pub const fn guest_size_of<T>() -> GuestUSize {
        std::mem::size_of::<T>() as GuestUSize
    }
    pub unsafe trait SafeRead: Copy {}

    #[derive(Copy, Clone)]
    pub struct Ptr<T, const MUT: bool> {
        offset: usize,
        _type: PhantomData<T>,
    }
    pub type MutVoidPtr = Ptr<(), true>;
    pub type ConstVoidPtr = Ptr<(), false>;
    impl<T, const MUT: bool> Ptr<T, MUT> {
        pub fn cast<U>(self) -> Ptr<U, MUT> {
            Ptr {
                offset: self.offset,
                _type: PhantomData,
            }
        }
    }
    impl<T> Ptr<T, true> {
        pub fn cast_const(self) -> Ptr<T, false> {
            Ptr {
                offset: self.offset,
                _type: PhantomData,
            }
        }
    }

    pub struct Mem(Vec<u8>);
    impl Mem {
        pub fn new() -> Self {
            Self(Vec::new())
        }
        pub fn alloc(&mut self, size: GuestUSize) -> MutVoidPtr {
            let offset = self.0.len();
            self.0.resize(offset + size as usize, 0);
            Ptr {
                offset,
                _type: PhantomData,
            }
        }
        pub fn read<T: SafeRead, const MUT: bool>(&self, ptr: Ptr<T, MUT>) -> T {
            assert!(ptr.offset + std::mem::size_of::<T>() <= self.0.len());
            unsafe { self.0.as_ptr().add(ptr.offset).cast::<T>().read_unaligned() }
        }
        pub fn write<T: SafeRead>(&mut self, ptr: Ptr<T, true>, value: T) {
            assert!(ptr.offset + std::mem::size_of::<T>() <= self.0.len());
            unsafe {
                self.0
                    .as_mut_ptr()
                    .add(ptr.offset)
                    .cast::<T>()
                    .write_unaligned(value)
            }
        }
    }
}

mod abi {
    use crate::mem::{ConstVoidPtr, GuestUSize, Mem, MutVoidPtr};
    pub trait GuestArg: std::fmt::Debug + Sized {
        const REG_COUNT: usize;
        fn from_regs(regs: &[u32]) -> Self;
        fn to_regs(self, regs: &mut [u32]);
    }
    impl GuestArg for f32 {
        const REG_COUNT: usize = 1;
        fn from_regs(regs: &[u32]) -> Self {
            f32::from_bits(regs[0])
        }
        fn to_regs(self, regs: &mut [u32]) {
            regs[0] = self.to_bits();
        }
    }
    pub trait GuestRet: std::fmt::Debug + Sized {
        const SIZE_IN_MEM: Option<GuestUSize> = None;
        // These also exist on GuestArg in the engine. Keeping both methods in
        // this stand-in catches ambiguous trait method calls in the real suite.
        fn from_regs(_regs: &[u32]) -> Self {
            panic!("unexpected register return")
        }
        fn to_regs(self, _regs: &mut [u32]) {
            panic!("unexpected register return")
        }
        fn from_mem(ptr: ConstVoidPtr, mem: &Mem) -> Self;
        fn to_mem(self, ptr: MutVoidPtr, mem: &mut Mem);
    }
    // Mirrors the engine's impl_GuestRet_for_large_struct! for this isolated
    // host test. The actual engine uses its own macro in src/abi.rs.
    macro_rules! impl_GuestRet_for_large_struct {
        ($for:ty) => {
            impl crate::abi::GuestRet for $for {
                const SIZE_IN_MEM: Option<crate::mem::GuestUSize> =
                    Some(crate::mem::guest_size_of::<$for>());
                fn from_mem(ptr: crate::mem::ConstVoidPtr, mem: &crate::mem::Mem) -> Self {
                    mem.read(ptr.cast::<Self>())
                }
                fn to_mem(self, ptr: crate::mem::MutVoidPtr, mem: &mut crate::mem::Mem) {
                    mem.write(ptr.cast::<Self>(), self)
                }
            }
        };
    }
    pub(crate) use impl_GuestRet_for_large_struct;
}

#[path = "../third_party/anyiose-engine/src/frameworks/uikit/ui_edge_insets.rs"]
mod ui_edge_insets;
