// Simulated UE (the phone side).
//
// Attaches to the base station, then drives along a straight road from
// one cell towards another while sending measurement reports. Signal
// strength follows a log distance path loss model, so at some point the
// second cell becomes stronger and the base station should hand the UE
// over. Finally the UE detaches.
//
// Exit codes: 0 success, 1 transport or protocol failure, 2 bad arguments,
// 3 attach rejected.

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "mobility_messages.hpp"
#include "session_messages.hpp"
#include "sigflow/endpoint.hpp"
#include "sigflow/transport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ran;

struct Cell {
    std::uint32_t id;
    double position_m;  // where the cell sits along the road
};

// Two cells 1 km apart. Matches the default --cell-id of the base station.
const std::vector<Cell> kCells = {{101, 0.0}, {102, 1000.0}};

// Log distance path loss: received power drops by 10 * n dB per decade of
// distance. The constants are picked so the numbers land in a realistic
// RSRP range, around -70 dBm next to a cell and -100 dBm a kilometre away.
// Cells stand 100 m off the road, so the UE is never right on top of one.
std::int16_t rsrp_dbm_x10(double along_road_m) {
    constexpr double kReferenceDbm = -10.0;  // at 1 m
    constexpr double kPathLossExponent = 3.0;
    constexpr double kOffsetFromRoadM = 100.0;
    const double distance = std::hypot(along_road_m, kOffsetFromRoadM);
    const double rsrp = kReferenceDbm - 10.0 * kPathLossExponent * std::log10(distance);
    return static_cast<std::int16_t>(std::lround(rsrp * 10.0));
}

mobility::CellMeasurement measure(const Cell& cell, double ue_position_m) {
    mobility::CellMeasurement m;
    m.cell_id = cell.id;
    m.rsrp_dbm_x10 = rsrp_dbm_x10(std::abs(cell.position_m - ue_position_m));
    // RSRQ is not used by the handover logic, a fixed plausible value is
    // good enough here.
    m.rsrq_db_x10 = -105;
    return m;
}

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9500;
    std::uint64_t imsi = 262011234567890ULL;
    std::string name = "sim-ue";
    int reports = 8;
    int interval_ms = 100;
    double speed_m_per_report = 120.0;
    int attach_attempts = 3;
};

class Ue {
public:
    Ue(const Options& options, sigflow::Endpoint& endpoint, const sim::Logger& log)
        : options_(options), endpoint_(endpoint), log_(log) {
        endpoint_.on<mobility::HandoverCommand>([this](const mobility::HandoverCommand& command) {
            log_.info("handover to cell ", command.target_cell_id, " (margin ", command.margin_db_x10 / 10.0, " dB)");
            serving_cell_ = command.target_cell_id;
            handovers_++;
        });
    }

    // Returns 0 on success, otherwise one of the exit codes listed on top.
    int run() {
        if (int code = attach(); code != 0) return code;

        double position = 0.0;
        for (int i = 0; i < options_.reports && !sim::stop_requested.load(); i++) {
            if (!send_report(position)) return 1;
            // Give the base station a moment to answer before moving on.
            // Frames that arrive here go to the handover handler.
            if (auto polled = endpoint_.poll(std::chrono::milliseconds(options_.interval_ms)); !polled) {
                log_.info("transport failed: ", polled.error().message());
                return 1;
            }
            position += options_.speed_m_per_report;
        }

        if (!detach()) return 1;
        log_.info("done, ", handovers_, " handover(s), now on cell ", serving_cell_);
        return 0;
    }

private:
    int attach() {
        session::AttachRequest request;
        request.imsi = options_.imsi;
        request.category = session::UeCategory::kCat4;
        request.device_name = options_.name;
        request.supported_bands = {1, 3, 7, 20};
        request.requested_bandwidth_mhz = 20;

        // UDP gives no delivery guarantee, so the request is simply sent
        // again when no answer shows up in time. The base station treats a
        // repeated IMSI as the same UE, which makes the retry safe.
        for (int attempt = 1; attempt <= options_.attach_attempts; attempt++) {
            log_.info("attach attempt ", attempt, " imsi=", request.imsi);
            if (auto sent = endpoint_.send(request); !sent) {
                log_.info("send failed: ", sent.error().message());
                return 1;
            }

            auto response = endpoint_.wait_for<session::AttachResponse>(1s);
            if (!response) {
                log_.info("transport failed: ", response.error().message());
                return 1;
            }
            if (!response->has_value()) continue;  // timed out, try again

            const auto& answer = **response;
            switch (answer.result) {
                case session::AttachResult::kAccepted:
                    ue_id_ = answer.ue_id.value_or(0);
                    serving_cell_ = answer.serving_cell_id.value_or(0);
                    log_.info("attached as ue_id=", ue_id_, " on cell ", serving_cell_);
                    return 0;
                case session::AttachResult::kRejectedCongestion:
                    log_.info("cell is congested, backing off");
                    std::this_thread::sleep_for(std::chrono::milliseconds(answer.retry_after_ms.value_or(500)));
                    continue;
                case session::AttachResult::kRejectedUnknownNetwork:
                    log_.info("attach rejected: ", session::to_string(answer.result));
                    return 3;
            }
        }
        log_.info("no answer after ", options_.attach_attempts, " attempts");
        return 1;
    }

    bool send_report(double position_m) {
        mobility::MeasurementReport report;
        report.ue_id = ue_id_;
        for (const Cell& cell : kCells) {
            auto measurement = measure(cell, position_m);
            if (cell.id == serving_cell_) {
                report.serving = measurement;
            } else {
                report.neighbours.push_back(measurement);
            }
        }

        log_.info("at ", position_m, " m: serving ", report.serving.rsrp_dbm_x10 / 10.0, " dBm");
        if (auto sent = endpoint_.send(report); !sent) {
            log_.info("send failed: ", sent.error().message());
            return false;
        }
        return true;
    }

    bool detach() {
        session::DetachRequest request;
        request.ue_id = ue_id_;
        request.cause = session::DetachCause::kUserRequest;

        for (int attempt = 1; attempt <= options_.attach_attempts; attempt++) {
            if (auto sent = endpoint_.send(request); !sent) return false;
            auto accept = endpoint_.wait_for<session::DetachAccept>(1s);
            if (!accept) return false;
            if (accept->has_value()) {
                log_.info("detached");
                return true;
            }
        }
        log_.info("detach was never confirmed");
        return false;
    }

    Options options_;
    sigflow::Endpoint& endpoint_;
    const sim::Logger& log_;
    std::uint32_t ue_id_ = 0;
    std::uint32_t serving_cell_ = 0;
    int handovers_ = 0;
};

void print_usage() {
    std::cout << "usage: sigflow-ue [--host H] [--port N] [--imsi N] [--name S] [--reports N]\n"
                 "                  [--interval-ms N] [--speed-m N]\n";
}

int run(int argc, char** argv) {
    sim::Args args(argc, argv, {"host", "port", "imsi", "name", "reports", "interval-ms", "speed-m"});
    if (args.help()) {
        print_usage();
        return 0;
    }

    Options options;
    options.host = args.get("host", options.host);
    options.port = args.get<std::uint16_t>("port", options.port);
    options.imsi = args.get<std::uint64_t>("imsi", options.imsi);
    options.name = args.get("name", options.name);
    options.reports = args.get<int>("reports", options.reports);
    options.interval_ms = args.get<int>("interval-ms", options.interval_ms);
    options.speed_m_per_report = args.get<int>("speed-m", 120);

    sim::Logger log("ue");
    sim::install_stop_handler();

    auto transport = sigflow::UdpTransport::connect(options.host, options.port);
    if (!transport) {
        log.info("cannot start: ", transport.error().message());
        return 1;
    }
    sigflow::Endpoint endpoint(**transport);
    return Ue(options, endpoint, log).run();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "sigflow-ue: " << error.what() << "\n";
        return 2;
    }
}
