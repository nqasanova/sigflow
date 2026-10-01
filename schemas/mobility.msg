// Measurement reporting and handover.
//
// Signal levels are sent as fixed point integers in tenths of a dB, so
// -95.5 dBm travels as -955. Real radio protocols avoid floats on the wire
// for the same reason: the precision is known up front and both sides
// round the same way.

namespace ran.mobility;

struct CellMeasurement {
    1: u32 cell_id;
    2: i16 rsrp_dbm_x10;  // reference signal received power
    3: i16 rsrq_db_x10;   // reference signal received quality
}

message MeasurementReport = 0x0201 {
    1: u32 ue_id;
    2: CellMeasurement serving;
    3: repeated CellMeasurement neighbours;
}

message HandoverCommand = 0x0202 {
    1: u32 ue_id;
    2: u32 target_cell_id;
    3: i16 margin_db_x10;  // how much stronger the target was
}
