// Compile the real option parser with minimal device/GLES stubs. This checks
// argument mapping, not actual rotation, graphics, networking or hardware clocks.
#![allow(dead_code)]
mod gles {
    #[derive(Clone)]
    pub enum GLESImplementation {
        X,
    }
    impl GLESImplementation {
        pub fn from_short_name(_: &str) -> Result<Self, ()> {
            Ok(Self::X)
        }
    }
}
mod window {
    #[derive(Clone, Copy, Debug, PartialEq)]
    pub enum DeviceOrientation {
        Portrait,
        PortraitUpsideDown,
        LandscapeLeft,
        LandscapeRight,
    }
    #[derive(Clone, Copy)]
    pub enum DeviceFamily {
        Family,
    }
    impl TryFrom<&str> for DeviceFamily {
        type Error = ();
        fn try_from(_: &str) -> Result<Self, ()> {
            Ok(Self::Family)
        }
    }
}
mod paths {
    pub fn user_data_base_path() -> std::path::PathBuf {
        std::env::temp_dir()
    }
}
#[path = "../third_party/anyiose-engine/src/options.rs"]
mod options;
#[test]
fn classic_offline_and_orientation_arguments_apply_to_actual_options() {
    let mut o = options::Options::default();
    assert!(!o.force_initial_orientation);
    assert_eq!(o.host_screen_size, None);
    assert_eq!(o.scale_hack.get(), 1);
    assert!(o.parse_argument("--allow-network-access").unwrap());
    assert!(o.network_access);
    for arg in [
        "--device-family=auto",
        "--screen-size=480x320",
        "--scale-hack=4",
        "--no-network-access",
        "--perf-hints",
        "--affinity=big",
        "--anyiose-initial-portrait",
    ] {
        assert!(o.parse_argument(arg).unwrap(), "{arg}");
    }
    assert_eq!(o.host_screen_size, Some((480, 320)));
    assert_eq!(o.scale_hack.get(), 4);
    assert!(!o.network_access && o.perf_hints && o.force_initial_orientation);
    assert_eq!(o.initial_orientation, window::DeviceOrientation::Portrait);
    assert_eq!(o.affinity.as_deref(), Some("big"));
    assert!(o.parse_argument("--landscape-left").unwrap());
    assert_eq!(
        o.initial_orientation,
        window::DeviceOrientation::LandscapeLeft
    );
}
