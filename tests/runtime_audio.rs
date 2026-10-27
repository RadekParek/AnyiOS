// Host-stub harness compiling actual AudioComponent and AUGraph entry points
// without SDL, a device, or game binaries. The AudioUnit/OpenAL calls are
// stubs: this exercises graph ownership and routing, not audio playback.
#![allow(
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    unused_imports,
    unused_variables
)]

#[macro_export]
macro_rules! log {
    ($($t:tt)*) => {{}};
}
#[macro_export]
macro_rules! log_dbg {
    ($($t:tt)*) => {{}};
}
#[macro_export]
macro_rules! export_c_func {
    ($($t:tt)*) => {
        ()
    };
}

mod abi {
    #[derive(Clone, Copy, Debug)]
    pub struct GuestFunction(u32);
    impl GuestFunction {
        pub fn from_addr_with_thumb_bit(addr: u32) -> Self {
            Self(addr)
        }
        pub fn addr_with_thumb_bit(self) -> u32 {
            self.0
        }
    }
}
mod audio {
    pub mod openal {
        pub mod al_types {
            pub type ALuint = u32;
        }
    }
}
mod dyld {
    pub type FunctionExports = &'static [()];
}
mod mem {
    use std::marker::PhantomData;
    #[repr(transparent)]
    pub struct Ptr<T, const M: bool>(u32, PhantomData<T>);
    impl<T, const M: bool> Copy for Ptr<T, M> {}
    impl<T, const M: bool> Clone for Ptr<T, M> {
        fn clone(&self) -> Self {
            *self
        }
    }
    impl<T, const M: bool> PartialEq for Ptr<T, M> {
        fn eq(&self, rhs: &Self) -> bool {
            self.0 == rhs.0
        }
    }
    impl<T, const M: bool> Eq for Ptr<T, M> {}
    impl<T, const M: bool> std::hash::Hash for Ptr<T, M> {
        fn hash<H: std::hash::Hasher>(&self, h: &mut H) {
            self.0.hash(h)
        }
    }
    impl<T, const M: bool> std::fmt::Debug for Ptr<T, M> {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            write!(f, "{:#x}", self.0)
        }
    }
    impl<T, const M: bool> Default for Ptr<T, M> {
        fn default() -> Self {
            Self::null()
        }
    }
    impl<T, const M: bool> Ptr<T, M> {
        pub const fn from_bits(value: u32) -> Self {
            Self(value, PhantomData)
        }
        pub const fn null() -> Self {
            Self::from_bits(0)
        }
        pub fn is_null(self) -> bool {
            self.0 == 0
        }
        pub fn cast<U>(self) -> Ptr<U, M> {
            Ptr::from_bits(self.0)
        }
        pub fn cast_const(self) -> Ptr<T, false> {
            Ptr::from_bits(self.0)
        }
    }
    pub type ConstPtr<T> = Ptr<T, false>;
    pub type MutPtr<T> = Ptr<T, true>;
    pub type ConstVoidPtr = ConstPtr<std::ffi::c_void>;
    pub type MutVoidPtr = MutPtr<std::ffi::c_void>;
    pub unsafe trait SafeRead: Sized {}
    pub trait SafeWrite: Sized {}
    impl<T: SafeRead> SafeWrite for T {}
    unsafe impl<T, const M: bool> SafeRead for Ptr<T, M> {}
    pub const fn guest_size_of<T>() -> u32 {
        std::mem::size_of::<T>() as u32
    }
    unsafe impl SafeRead for u8 {}
    unsafe impl SafeRead for u32 {}
    unsafe impl SafeRead for i32 {}
    #[derive(Default)]
    pub struct Mem {
        next: u32,
        values: std::collections::HashMap<u32, Vec<u8>>,
    }
    impl Mem {
        pub fn read<T: SafeRead, const M: bool>(&self, p: Ptr<T, M>) -> T {
            let bytes = self.values.get(&p.0).expect("valid allocated test pointer");
            assert_eq!(bytes.len(), std::mem::size_of::<T>());
            let mut value = std::mem::MaybeUninit::<T>::uninit();
            // SafeRead is only implemented for POD guest structs in this harness.
            unsafe {
                std::ptr::copy_nonoverlapping(
                    bytes.as_ptr(),
                    value.as_mut_ptr() as *mut u8,
                    bytes.len(),
                );
                value.assume_init()
            }
        }
        pub fn write<T: SafeWrite>(&mut self, p: MutPtr<T>, v: T) {
            let bytes = unsafe {
                std::slice::from_raw_parts(&v as *const T as *const u8, std::mem::size_of::<T>())
            };
            self.values.insert(p.0, bytes.to_vec());
        }
        pub fn alloc_and_write<T: SafeWrite>(&mut self, v: T) -> MutPtr<T> {
            self.next += 16;
            let ptr = MutPtr::from_bits(self.next);
            self.write(ptr, v);
            ptr
        }
        pub fn free(&mut self, p: MutVoidPtr) {
            self.values.remove(&p.0);
        }
    }
}
mod frameworks {
    pub mod audio_toolbox {
        pub(crate) use crate::{audio_components, audio_unit};
    }
    pub mod carbon_core {
        pub type OSStatus = i32;
        pub const paramErr: i32 = -50;
    }
    pub mod core_audio_types {
        pub const fn fourcc(b: &[u8; 4]) -> u32 {
            u32::from_be_bytes(*b)
        }
        pub const kAudioFormatFlagIsAlignedHigh: u32 = 1;
        pub const kAudioFormatFlagIsFloat: u32 = 2;
        pub const kAudioFormatFlagIsPacked: u32 = 4;
        pub const kAudioFormatFlagIsSignedInteger: u32 = 8;
        pub const kAudioFormatLinearPCM: u32 = 16;
        #[derive(Clone, Copy)]
        pub struct AudioStreamBasicDescription {
            pub sample_rate: f64,
            pub format_id: u32,
            pub format_flags: u32,
            pub bytes_per_packet: u32,
            pub frames_per_packet: u32,
            pub bytes_per_frame: u32,
            pub channels_per_frame: u32,
            pub bits_per_channel: u32,
            pub _reserved: u32,
        }
    }
    #[derive(Default)]
    pub struct State {
        pub audio_toolbox: AudioToolbox,
    }
    #[derive(Default)]
    pub struct AudioToolbox {
        pub audio_components: crate::audio_components::State,
        pub au_graph: crate::au_graph::State,
        pub registrations: Vec<crate::audio_components::AudioComponentInstance>,
    }
}
mod environment {
    #[derive(Default)]
    pub struct Environment {
        pub mem: crate::mem::Mem,
        pub framework_state: crate::frameworks::State,
    }
}
fn record_audio_event(_env: &mut environment::Environment, _event: String) {}
mod audio_unit {
    use crate::{audio_components as components, environment::Environment};
    pub type AudioUnit = components::AudioComponentInstance;

    pub fn AudioUnitInitialize_impl(env: &mut Environment, unit: AudioUnit) -> i32 {
        let Some(obj) = env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .get_mut(&unit)
        else {
            return -50;
        };
        if obj.mark_initialized() {
            env.framework_state.audio_toolbox.registrations.push(unit);
        }
        0
    }
    pub fn AudioUnitUninitialize(env: &mut Environment, unit: AudioUnit) -> i32 {
        let Some(obj) = env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .get_mut(&unit)
        else {
            return -50;
        };
        obj.mark_uninitialized();
        obj.take_sources();
        env.framework_state
            .audio_toolbox
            .registrations
            .retain(|u| *u != unit);
        0
    }
    pub fn dispose_audio_unit(env: &mut Environment, unit: AudioUnit) {
        let _ = AudioUnitUninitialize(env, unit);
    }
    pub fn setup_audio_unit_for_render(env: &mut Environment, unit: AudioUnit) {
        if let Some(obj) = env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .get_mut(&unit)
        {
            obj.started = true;
        }
    }
    pub fn stop_audio_unit_for_graph(env: &mut Environment, unit: AudioUnit) {
        if let Some(obj) = env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .get_mut(&unit)
        {
            obj.take_sources();
        }
    }
}
#[path = "../third_party/anyiose-engine/src/frameworks/audio_toolbox/audio_components.rs"]
mod audio_components;

#[path = "../third_party/anyiose-engine/src/frameworks/audio_toolbox/au_graph.rs"]
mod au_graph;

#[cfg(test)]
mod graph_tests {
    use super::*;
    use au_graph::*;
    use audio_components::AURenderCallbackStruct;
    use mem::{ConstVoidPtr, MutPtr};

    fn create_graph(env: &mut environment::Environment) -> AUGraph {
        let out = env.mem.alloc_and_write(AUGraph::null());
        assert_eq!(NewAUGraph(env, out), 0);
        env.mem.read(out)
    }
    fn add_mixer(env: &mut environment::Environment, graph: AUGraph) -> AUNode {
        let desc = env.mem.alloc_and_write(ComponentDesc {
            component_type: u32::from_be_bytes(*b"aumx"),
            component_sub_type: u32::from_be_bytes(*b"mcmx"),
            component_manufacturer: u32::from_be_bytes(*b"appl"),
            component_flags: 0,
            component_flags_mask: 0,
        });
        let out = env.mem.alloc_and_write(0i32);
        assert_eq!(AUGraphAddNode(env, graph, desc.cast_const(), out), 0);
        env.mem.read(out)
    }
    fn node_unit(
        env: &mut environment::Environment,
        graph: AUGraph,
        node: AUNode,
    ) -> audio_unit::AudioUnit {
        let out = env.mem.alloc_and_write(audio_unit::AudioUnit::null());
        assert_eq!(AUGraphNodeInfo(env, graph, node, MutPtr::null(), out), 0);
        env.mem.read(out)
    }
    fn callback(env: &mut environment::Environment) -> mem::ConstPtr<AURenderCallbackStruct> {
        env.mem
            .alloc_and_write(AURenderCallbackStruct {
                input_proc: abi::GuestFunction::from_addr_with_thumb_bit(0x1001),
                input_proc_ref_con: ConstVoidPtr::null(),
            })
            .cast_const()
    }
    fn has_bus(env: &environment::Environment, unit: audio_unit::AudioUnit, bus: u32) -> bool {
        env.framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances[&unit]
            .mixer_buses
            .get(&bus)
            .and_then(|bus| bus.render_callback)
            .is_some()
    }

    // Apple's graph lifecycle retains nodes/interactions but frees instantiated
    // units on close: Core Audio Overview, Common Tasks, Hosting Audio Units.
    #[test]
    fn graph_close_reopen_keeps_callback_and_disposes_old_units() {
        let mut env = environment::Environment::default();
        let graph = create_graph(&mut env);
        let node = add_mixer(&mut env, graph);
        let cb = callback(&mut env);
        assert_eq!(AUGraphSetNodeInputCallback(&mut env, graph, node, 3, cb), 0);
        assert_eq!(AUGraphOpen(&mut env, graph), 0);
        let first = node_unit(&mut env, graph, node);
        assert!(!first.is_null() && has_bus(&env, first, 3));
        assert_eq!(AUGraphOpen(&mut env, graph), 0);
        assert_eq!(node_unit(&mut env, graph, node), first);
        assert_eq!(AUGraphInitialize(&mut env, graph), 0);
        assert_eq!(AUGraphInitialize(&mut env, graph), 0);
        assert_eq!(env.framework_state.audio_toolbox.registrations.len(), 1);
        assert_eq!(AUGraphStart(&mut env, graph), 0);
        assert!(
            env.framework_state
                .audio_toolbox
                .audio_components
                .audio_component_instances[&first]
                .started
        );
        assert_eq!(AUGraphStop(&mut env, graph), 0);
        assert!(
            !env.framework_state
                .audio_toolbox
                .audio_components
                .audio_component_instances[&first]
                .started
        );
        assert_eq!(AUGraphClose(&mut env, graph), 0);
        assert!(env.framework_state.audio_toolbox.registrations.is_empty());
        assert!(!env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .contains_key(&first));
        assert_eq!(AUGraphOpen(&mut env, graph), 0);
        let second = node_unit(&mut env, graph, node);
        assert_ne!(first, second);
        assert!(has_bus(&env, second, 3));
        assert_eq!(AUGraphDisconnectNodeInput(&mut env, graph, node, 3), 0);
        assert!(!has_bus(&env, second, 3));
        assert_eq!(AUGraphClose(&mut env, graph), 0);
        assert_eq!(AUGraphOpen(&mut env, graph), 0);
        let third = node_unit(&mut env, graph, node);
        assert!(!has_bus(&env, third, 3));
        assert_eq!(DisposeAUGraph(&mut env, graph), 0);
        assert!(!env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .contains_key(&third));
    }

    #[test]
    fn nodes_added_to_open_graph_are_instantiated_and_removed() {
        let mut env = environment::Environment::default();
        let graph = create_graph(&mut env);
        assert_eq!(AUGraphOpen(&mut env, graph), 0);
        let node = add_mixer(&mut env, graph);
        let unit = node_unit(&mut env, graph, node);
        assert!(!unit.is_null());
        let cb = callback(&mut env);
        assert_eq!(AUGraphSetNodeInputCallback(&mut env, graph, node, 0, cb), 0);
        assert!(has_bus(&env, unit, 0));
        assert_eq!(AUGraphRemoveNode(&mut env, graph, node), 0);
        assert!(!env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .contains_key(&unit));
        assert_eq!(DisposeAUGraph(&mut env, graph), 0);
    }
    #[test]
    fn graph_stop_releases_bridged_output_without_disposing_it() {
        let mut env = environment::Environment::default();
        let graph = create_graph(&mut env);
        let mixer = add_mixer(&mut env, graph);
        let desc = env.mem.alloc_and_write(ComponentDesc {
            component_type: u32::from_be_bytes(*b"auou"),
            component_sub_type: u32::from_be_bytes(*b"rioc"),
            component_manufacturer: u32::from_be_bytes(*b"appl"),
            component_flags: 0,
            component_flags_mask: 0,
        });
        let node_out = env.mem.alloc_and_write(0i32);
        assert_eq!(
            AUGraphAddNode(&mut env, graph, desc.cast_const(), node_out),
            0
        );
        let output_node = env.mem.read(node_out);
        assert_eq!(
            AUGraphConnectNodeInput(&mut env, graph, mixer, 0, output_node, 0),
            0
        );
        assert_eq!(AUGraphOpen(&mut env, graph), 0);
        assert_eq!(AUGraphInitialize(&mut env, graph), 0);
        let outside = audio_components::create_audio_unit_instance(
            &mut env,
            Some((
                u32::from_be_bytes(*b"auou"),
                u32::from_be_bytes(*b"rioc"),
                u32::from_be_bytes(*b"appl"),
            )),
        );
        assert!(start_graph_for_output(&mut env, outside));
        assert!(
            env.framework_state
                .audio_toolbox
                .audio_components
                .audio_component_instances[&outside]
                .started
        );
        assert_eq!(AUGraphStop(&mut env, graph), 0);
        assert!(
            !env.framework_state
                .audio_toolbox
                .audio_components
                .audio_component_instances[&outside]
                .started
        );
        assert!(start_graph_for_output(&mut env, outside));
        assert_eq!(AUGraphUninitialize(&mut env, graph), 0);
        assert!(
            !env.framework_state
                .audio_toolbox
                .audio_components
                .audio_component_instances[&outside]
                .started
        );
        assert_eq!(DisposeAUGraph(&mut env, graph), 0);
        assert!(env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .contains_key(&outside));
    }
}
