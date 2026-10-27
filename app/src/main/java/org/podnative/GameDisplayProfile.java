package org.podnative;

/** Guest layout in iOS points; presentation resolution is a separate setting. */
final class GameDisplayProfile {
    static final int AUTO = 0;
    static final int CLASSIC_PHONE = 1;
    static final int TALL_PHONE = 2;
    static final int TABLET = 3;

    private GameDisplayProfile() {}

    static String argument(int profile) {
        switch (profile) {
            case CLASSIC_PHONE:
                return "--device-family=iphone-3gs";
            case TALL_PHONE:
                return "--device-family=iphone-5";
            case TABLET:
                return "--device-family=ipad-2";
            default:
                return "--device-family=auto";
        }
    }

    static String label(int profile) {
        switch (profile) {
            case CLASSIC_PHONE:
                return "Classic phone · 320×480";
            case TALL_PHONE:
                return "Tall phone · 320×568";
            case TABLET:
                return "Classic tablet · 768×1024";
            default:
                return "Automatic · match display";
        }
    }
}
