// Session management between a simulated UE and a base station.
//
// Loosely based on the LTE attach and detach procedures, but heavily
// simplified: no security, no bearers, no NAS/RRC split. Just enough to
// have a real request/response exchange running over the framework.

namespace ran.session;

enum UeCategory : u8 {
    CAT_M1 = 0;
    CAT_1 = 1;
    CAT_4 = 4;
    CAT_6 = 6;
}

enum AttachResult : u8 {
    ACCEPTED = 0;
    REJECTED_UNKNOWN_NETWORK = 1;
    REJECTED_CONGESTION = 2;
}

enum DetachCause : u8 {
    USER_REQUEST = 0;
    POWER_OFF = 1;
}

message AttachRequest = 0x0101 {
    1: u64 imsi;
    2: UeCategory category;
    3: string device_name;
    4: repeated u16 supported_bands;
    5: optional u16 requested_bandwidth_mhz;
}

message AttachResponse = 0x0102 {
    1: AttachResult result;
    2: optional u32 ue_id;            // only present when accepted
    3: optional u32 serving_cell_id;  // only present when accepted
    4: optional u16 retry_after_ms;   // only present on congestion
}

message DetachRequest = 0x0103 {
    1: u32 ue_id;
    2: DetachCause cause;
}

message DetachAccept = 0x0104 {
    1: u32 ue_id;
}
