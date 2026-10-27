// Host-stub harness compiling the actual AudioComponent, AudioUnit, and
// AUGraph code without SDL, a device, or game binaries. The OpenAL backend,
// guest callbacks/memory, PCM decoder, microphone, Foundation, and run loop
// are mocked: this checks lifecycle/reentrancy, not audio playback or ABI.
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
macro_rules! log_once {
    ($($t:tt)*) => {{}};
}
#[macro_export]
macro_rules! msg {
    ($($t:tt)*) => {
        crate::objc::id::null()
    };
}
#[macro_export]
macro_rules! msg_class {
    ($($t:tt)*) => {
        crate::objc::id::null()
    };
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
    #[repr(transparent)]
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub struct GuestFunction(u32);
    impl GuestFunction {
        pub fn from_addr_with_thumb_bit(addr: u32) -> Self {
            Self(addr)
        }
        pub fn addr_with_thumb_bit(self) -> u32 {
            self.0
        }
        pub fn to_ptr(self) -> crate::mem::ConstVoidPtr {
            crate::mem::ConstPtr::from_bits(self.0)
        }
    }
    pub trait CallFromHost {
        fn call_from_host<A, R: Default>(
            &self,
            env: &mut crate::environment::Environment,
            args: A,
        ) -> R;
    }
    impl CallFromHost for GuestFunction {
        fn call_from_host<A, R: Default>(
            &self,
            env: &mut crate::environment::Environment,
            _args: A,
        ) -> R {
            env.callback_trace.push(self.0);
            let action = std::mem::take(&mut env.callback_action);
            let unit = env.callback_unit;
            // The action simulates guest code entering the emulator again
            // while the callback is in flight; no guest CPU is run here.
            match action {
                1 => crate::audio_unit::render_audio_unit(env, unit),
                2 => crate::audio_unit::stop_audio_unit_for_graph(env, unit),
                3 => {
                    if let Some(obj) = env
                        .framework_state
                        .audio_toolbox
                        .audio_components
                        .audio_component_instances
                        .get_mut(&unit)
                    {
                        obj.set_render_callback(true, 0, None);
                    }
                }
                4 => {
                    crate::audio_components::AudioComponentInstanceDispose(env, unit);
                }
                _ => {}
            }
            R::default()
        }
    }
}
mod audio {
    pub mod openal {
        pub mod al_types {
            pub type ALuint = u32;
            pub type ALsizei = i32;
            pub type ALvoid = std::ffi::c_void;
        }
        pub const AL_BUFFERS_PROCESSED: i32 = 1;
        pub const AL_BUFFERS_QUEUED: i32 = 2;
        pub const AL_GAIN: i32 = 3;
        pub const AL_PITCH: i32 = 4;
        pub const AL_PLAYING: i32 = 5;
        pub const AL_SOURCE_STATE: i32 = 6;
        pub const AL_FORMAT_STEREO16: i32 = 7;
        #[derive(Default)]
        pub struct OpenALManager {
            pub next: std::cell::Cell<u32>,
            pub created: std::cell::RefCell<Vec<u32>>,
            pub stopped: std::cell::RefCell<Vec<u32>>,
            pub deleted: std::cell::RefCell<Vec<u32>>,
            pub queued: std::cell::Cell<u32>,
        }
        pub struct OpenAL<'a>(pub &'a OpenALManager);
        impl OpenAL<'_> {
            pub unsafe fn Sourcef(&self, _s: u32, _p: i32, _v: f32) {}
            pub unsafe fn Source3f(&self, _s: u32, _p: i32, _x: f32, _y: f32, _z: f32) {}
            pub unsafe fn SourceStop(&self, s: u32) {
                self.0.stopped.borrow_mut().push(s);
            }
            pub unsafe fn SourcePlay(&self, _s: u32) {}
            pub unsafe fn GetSourcei(&self, _s: u32, _p: i32, _v: *mut i32) {}
            pub unsafe fn SourceUnqueueBuffers(&self, _s: u32, _n: i32, _v: *mut u32) {}
            pub unsafe fn SourceQueueBuffers(&self, _s: u32, _n: i32, _v: *const u32) {
                self.0.queued.set(self.0.queued.get() + 1);
            }
            pub unsafe fn DeleteSources(&self, _n: i32, v: *const u32) {
                self.0.deleted.borrow_mut().push(*v);
            }
            pub unsafe fn DeleteBuffers(&self, _n: i32, _v: *const u32) {}
            pub unsafe fn GenBuffers(&self, _n: i32, v: *mut u32) {
                *v = 1;
            }
            pub unsafe fn GenSources(&self, _n: i32, v: *mut u32) {
                let next = self.0.next.get() + 1;
                self.0.next.set(next);
                *v = next;
                self.0.created.borrow_mut().push(next);
            }
            pub unsafe fn BufferData(
                &self,
                _b: u32,
                _fmt: i32,
                _data: *const std::ffi::c_void,
                _len: i32,
                _rate: i32,
            ) {
            }
        }
        #[derive(Default)]
        pub struct Lazy;
        impl Lazy {
            pub fn make_al_context_current<'a>(
                &'a mut self,
                _m: &'a mut OpenALManager,
            ) -> OpenAL<'a> {
                OpenAL(_m)
            }
        }
    }
}
mod android_media {
    pub const MIC_SAMPLE_RATE: u32 = 48000;
    pub fn start_mic() {}
    pub fn stop_mic() {}
    pub fn has_microphone() -> bool {
        false
    }
    pub fn read_mic_chunk() -> Vec<i16> {
        vec![]
    }
}
mod objc {
    pub type id = crate::mem::MutVoidPtr;
    pub use crate::{msg, msg_class};
    pub fn autorelease(_env: &mut crate::environment::Environment, v: id) -> id {
        v
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
        pub fn cast_void(self) -> Ptr<std::ffi::c_void, M> {
            Ptr::from_bits(self.0)
        }
    }
    impl<T, const M: bool> std::ops::Add<u32> for Ptr<T, M> {
        type Output = Self;
        fn add(self, delta: u32) -> Self {
            Self::from_bits(self.0 + delta * std::mem::size_of::<T>() as u32)
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
    unsafe impl SafeRead for f32 {}
    unsafe impl SafeRead for f64 {}
    unsafe impl SafeRead for u64 {}
    unsafe impl SafeRead for bool {}
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
        pub fn alloc(&mut self, n: u32) -> MutVoidPtr {
            self.next += 16384;
            self.values.insert(self.next, vec![0; n as usize]);
            MutPtr::from_bits(self.next)
        }
        pub fn bytes_at<const M: bool>(&self, p: Ptr<u8, M>, n: u32) -> &[u8] {
            &self.values.get(&p.0).expect("mock read buffer")[..n as usize]
        }
        pub fn bytes_at_mut(&mut self, p: MutPtr<u8>, n: u32) -> &mut [u8] {
            &mut self.values.get_mut(&p.0).expect("mock write buffer")[..n as usize]
        }
    }
}
mod frameworks {
    pub mod audio_toolbox {
        pub(crate) use crate::{au_graph, audio_components, audio_unit};
        pub mod audio_queue {
            use super::super::core_audio_types::AudioStreamBasicDescription as F;
            pub fn log_if_broken_audio_format(_f: &F) {}
            pub fn decode_buffer(
                m: &crate::mem::Mem,
                f: &F,
                p: crate::mem::MutPtr<u8>,
                n: u32,
                d: &[()],
            ) -> (i32, i32, Vec<u8>) {
                decode_buffer_from_bytes(f, m.bytes_at(p, n), d)
            }
            pub fn decode_buffer_from_bytes(f: &F, b: &[u8], _d: &[()]) -> (i32, i32, Vec<u8>) {
                if f.bits_per_channel == 32
                    && f.format_flags & super::super::core_audio_types::kAudioFormatFlagIsFloat != 0
                {
                    let out: Vec<u8> = b
                        .chunks_exact(4)
                        .flat_map(|x| {
                            ((f32::from_le_bytes(x.try_into().unwrap()) * 32767.0) as i16)
                                .to_le_bytes()
                        })
                        .collect();
                    (7, f.sample_rate as i32, out)
                } else {
                    (7, f.sample_rate as i32, b.to_vec())
                }
            }
        }
    }
    pub mod carbon_core {
        pub type OSStatus = i32;
        pub const paramErr: i32 = -50;
    }
    pub mod core_audio_types {
        pub const fn fourcc(b: &[u8; 4]) -> u32 {
            u32::from_be_bytes(*b)
        }
        pub const kAudioFormatFlagIsFloat: u32 = 1;
        pub const kAudioFormatFlagIsBigEndian: u32 = 2;
        pub const kAudioFormatFlagIsSignedInteger: u32 = 4;
        pub const kAudioFormatFlagIsPacked: u32 = 8;
        pub const kAudioFormatFlagIsAlignedHigh: u32 = 16;
        pub const kAudioFormatFlagIsNonInterleaved: u32 = 32;
        pub const kAudioFormatLinearPCM: u32 = u32::from_be_bytes(*b"lpcm");
        #[derive(Clone, Copy)]
        #[repr(C, packed)]
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
        unsafe impl crate::mem::SafeRead for AudioStreamBasicDescription {}
        #[repr(C, packed)]
        pub struct SMPTETime {
            pub subframes: i16,
            pub subframe_divisor: i16,
            pub counter: u32,
            pub type_: u32,
            pub flags: u32,
            pub hours: i16,
            pub minutes: i16,
            pub seconds: i16,
            pub frames: i16,
        }
        #[repr(C, packed)]
        pub struct AudioTimeStamp {
            pub sample_time: f64,
            pub host_time: u64,
            pub rate_scalar: f64,
            pub world_clock_type: u64,
            pub SMPTE_time: SMPTETime,
            pub flags: u32,
            pub _reserved: u32,
        }
        unsafe impl crate::mem::SafeRead for AudioTimeStamp {}
    }
    pub mod core_foundation {
        pub mod cf_run_loop {
            pub fn CFRunLoopGetMain(_e: &mut crate::environment::Environment) -> u32 {
                1
            }
        }
    }
    pub mod foundation {
        pub mod ns_string {
            pub fn from_rust_string(
                _e: &mut crate::environment::Environment,
                _s: String,
            ) -> crate::objc::id {
                crate::objc::id::null()
            }
        }
        pub mod ns_dictionary {
            pub fn dict_from_keys_and_objects(
                _e: &mut crate::environment::Environment,
                _a: &[(crate::objc::id, crate::objc::id)],
            ) -> crate::objc::id {
                crate::objc::id::null()
            }
        }
        pub mod ns_run_loop {
            pub fn add_audio_unit(
                e: &mut crate::environment::Environment,
                _r: u32,
                u: crate::audio_unit::AudioUnit,
            ) {
                e.framework_state.audio_toolbox.registrations.push(u);
            }
            pub fn remove_audio_unit(
                e: &mut crate::environment::Environment,
                _r: u32,
                u: crate::audio_unit::AudioUnit,
            ) -> bool {
                e.framework_state
                    .audio_toolbox
                    .registrations
                    .retain(|x| *x != u);
                true
            }
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
        pub al_context: crate::audio::openal::Lazy,
        pub audio_session: AudioSession,
    }
    #[derive(Default)]
    pub struct AudioSession {
        pub current_hardware_sample_rate: f64,
        pub current_hardware_io_buffer_duration: f32,
    }
    impl AudioToolbox {
        pub fn make_al_context_current<'a>(
            &'a mut self,
            m: &'a mut crate::audio::openal::OpenALManager,
        ) -> crate::audio::openal::OpenAL<'a> {
            self.al_context.make_al_context_current(m)
        }
    }
}
mod environment {
    pub struct GuestClock;
    impl GuestClock {
        pub fn now(&self) -> std::time::Instant {
            std::time::Instant::now()
        }
    }
    pub struct Bundle;
    impl Bundle {
        pub fn bundle_identifier(&self) -> &str {
            "test"
        }
    }

    pub struct Environment {
        pub mem: crate::mem::Mem,
        pub framework_state: crate::frameworks::State,
        pub bundle: Bundle,
        pub openal_manager: crate::audio::openal::OpenALManager,
        pub startup_time: std::time::Instant,
        pub guest_clock: GuestClock,
        pub callback_unit: crate::audio_unit::AudioUnit,
        pub callback_action: u8,
        pub callback_trace: Vec<u32>,
    }
    impl Default for Environment {
        fn default() -> Self {
            Self {
                mem: Default::default(),
                framework_state: Default::default(),
                bundle: Bundle,
                openal_manager: Default::default(),
                startup_time: std::time::Instant::now(),
                guest_clock: GuestClock,
                callback_unit: crate::mem::MutPtr::null(),
                callback_action: 0,
                callback_trace: vec![],
            }
        }
    }
}
fn record_audio_event(_env: &mut environment::Environment, _event: String) {}
#[path = "../third_party/anyiose-engine/src/frameworks/audio_toolbox/audio_components.rs"]
mod audio_components;
#[path = "../third_party/anyiose-engine/src/frameworks/audio_toolbox/audio_unit.rs"]
mod audio_unit;

#[path = "../third_party/anyiose-engine/src/frameworks/audio_toolbox/au_graph.rs"]
mod au_graph;

#[cfg(test)]
mod unit_runtime_tests {
    use super::*;
    use crate::audio_components::AURenderCallbackStruct;
    use crate::audio_components::{
        AudioComponentInstanceDispose, AudioComponentInstanceHostObject,
    };
    use crate::audio_unit::AudioUnit;
    fn setup(mixer: bool, action: u8, bus_count: u32) -> (environment::Environment, AudioUnit) {
        let mut env = environment::Environment::default();
        let unit: AudioUnit = env.mem.alloc_and_write(0u32).cast();
        env.callback_unit = unit;
        env.callback_action = action;
        let mut host = AudioComponentInstanceHostObject::default();
        host.is_3d_mixer = mixer;
        for bus in 0..bus_count {
            host.set_render_callback(
                true,
                bus,
                Some(AURenderCallbackStruct {
                    input_proc: abi::GuestFunction::from_addr_with_thumb_bit(0x300 + bus),
                    input_proc_ref_con: crate::mem::ConstVoidPtr::null(),
                }),
            );
        }
        env.framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .insert(unit, host);
        assert_eq!(audio_unit::AudioUnitInitialize_impl(&mut env, unit), 0);
        assert_eq!(audio_unit::AudioUnitInitialize_impl(&mut env, unit), 0);
        assert_eq!(env.framework_state.audio_toolbox.registrations.len(), 1);
        audio_unit::setup_audio_unit_for_render(&mut env, unit);
        (env, unit)
    }
    #[test]
    fn remote_io_unit_callback_reentry_is_guarded() {
        let (mut env, unit) = setup(false, 1, 1);
        assert!(env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances[&unit]
            .mixer_buses
            .is_empty());
        let (mut baseline, baseline_unit) = setup(false, 0, 1);
        audio_unit::render_audio_unit(&mut baseline, baseline_unit);
        assert!(!baseline.callback_trace.is_empty());
        audio_unit::render_audio_unit(&mut env, unit);
        assert_eq!(env.callback_trace, baseline.callback_trace); // reentry produced no extra callbacks
        assert!(
            !env.framework_state
                .audio_toolbox
                .audio_components
                .audio_component_instances[&unit]
                .is_running_handler
        );
        audio_unit::stop_audio_unit_for_graph(&mut env, unit);
        assert_eq!(env.openal_manager.created.borrow().len(), 1);
        assert_eq!(
            env.openal_manager.created.borrow().as_slice(),
            env.openal_manager.deleted.borrow().as_slice()
        );
        assert_eq!(audio_unit::AudioUnitUninitialize(&mut env, unit), 0);
        assert!(env.framework_state.audio_toolbox.registrations.is_empty());
    }
    #[test]
    fn mixer_callback_stop_cancels_remaining_callbacks_and_releases_all_sources() {
        let (mut env, unit) = setup(true, 2, 2);
        assert_eq!(env.openal_manager.created.borrow().len(), 3);
        audio_unit::render_audio_unit(&mut env, unit);
        assert_eq!(env.callback_trace.len(), 1);
        assert_eq!(env.openal_manager.queued.get(), 0);
        let host = &env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances[&unit];
        assert!(!host.started);
        assert!(!host.is_running_handler);
        assert!(host.al_source.is_none());
        assert!(host.mixer_buses.values().all(|bus| bus.al_source.is_none()));
        let mut created = env.openal_manager.created.borrow().clone();
        created.sort();
        let mut deleted = env.openal_manager.deleted.borrow().clone();
        deleted.sort();
        assert_eq!(created, deleted);
        audio_unit::stop_audio_unit_for_graph(&mut env, unit);
        assert_eq!(env.openal_manager.deleted.borrow().len(), 3);
    }
    #[test]
    fn callback_dispose_prevents_post_disposal_render() {
        let (mut env, unit) = setup(true, 4, 2);
        audio_unit::render_audio_unit(&mut env, unit);
        assert_eq!(env.callback_trace.len(), 1);
        assert!(!env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances
            .contains_key(&unit));
        assert_eq!(env.openal_manager.queued.get(), 0);
        assert_eq!(env.openal_manager.deleted.borrow().len(), 3);
        assert!(env.framework_state.audio_toolbox.registrations.is_empty());
    }
    #[test]
    fn callback_disconnect_cancels_the_in_flight_bus_plan() {
        let (mut env, unit) = setup(true, 3, 1);
        audio_unit::render_audio_unit(&mut env, unit);
        assert_eq!(env.callback_trace.len(), 1);
        assert_eq!(env.openal_manager.queued.get(), 0);
        let host = &env
            .framework_state
            .audio_toolbox
            .audio_components
            .audio_component_instances[&unit];
        assert!(host.started);
        assert!(!host.is_running_handler);
        assert!(host.mixer_buses[&0].render_callback.is_none());
        audio_unit::render_audio_unit(&mut env, unit);
        assert_eq!(env.callback_trace.len(), 1);
        assert_eq!(AudioComponentInstanceDispose(&mut env, unit), 0);
        assert_eq!(env.openal_manager.deleted.borrow().len(), 2);
    }
}
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
