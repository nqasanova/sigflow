// Simulated base station.
//
// Accepts attach requests, keeps a context per attached UE, watches the
// measurement reports and triggers a handover when a neighbour cell is
// clearly stronger than the serving one. The decision rule is a stripped
// down version of the LTE "A3" event: neighbour better than serving by a
// fixed margin.

#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <string>

#include "common.hpp"
#include "mobility_messages.hpp"
#include "session_messages.hpp"
#include "sigflow/endpoint.hpp"
#include "sigflow/transport.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ran;

struct Config {
    std::uint16_t port = 9500;
    std::uint32_t cell_id = 101;
    std::uint32_t plmn = 26201;  // MCC 262 (Germany) + MNC 01
    std::size_t max_ues = 8;
    std::int16_t handover_margin_db_x10 = 30;
};

struct UeContext {
    std::uint64_t imsi = 0;
    std::uint32_t serving_cell = 0;
    std::uint32_t handovers = 0;
};

// An IMSI is 15 digits: 3 for the country (MCC), usually 2 for the
// network (MNC) and 10 for the subscriber. Dividing off the subscriber
// part leaves MCC+MNC, which is all the network check needs.
constexpr std::uint32_t plmn_of(std::uint64_t imsi) {
    return static_cast<std::uint32_t>(imsi / 10'000'000'000ULL);
}

class BaseStation {
public:
    BaseStation(const Config& config, sigflow::Endpoint& endpoint, const sim::Logger& log)
        : config_(config), endpoint_(endpoint), log_(log) {
        endpoint_.on<session::AttachRequest>([this](const auto& m) { on_attach(m); });
        endpoint_.on<session::DetachRequest>([this](const auto& m) { on_detach(m); });
        endpoint_.on<mobility::MeasurementReport>([this](const auto& m) { on_report(m); });

        endpoint_.on_decode_error([this](const sigflow::Frame& frame, const sigflow::Error& error) {
            log_.info("dropped frame id=0x", std::hex, frame.header.message_id, std::dec, ": ", error.message());
        });
        endpoint_.on_unhandled([this](const sigflow::Frame& frame) {
            log_.info("no handler for message id 0x", std::hex, frame.header.message_id, std::dec);
        });
    }

private:
    void on_attach(const session::AttachRequest& request) {
        log_.info("attach request imsi=", request.imsi, " category=", session::to_string(request.category), " device='",
                  request.device_name, "'");

        session::AttachResponse response;
        if (plmn_of(request.imsi) != config_.plmn) {
            response.result = session::AttachResult::kRejectedUnknownNetwork;
            reply(response);
            log_.info("rejected imsi=", request.imsi, ": foreign network ", plmn_of(request.imsi));
            return;
        }

        // The UE retransmits its request if a response gets lost. Looking
        // the IMSI up first makes the procedure idempotent: a duplicate
        // request gets the same ue_id instead of a second context.
        std::uint32_t ue_id = find_by_imsi(request.imsi);
        if (ue_id == 0) {
            if (contexts_.size() >= config_.max_ues) {
                response.result = session::AttachResult::kRejectedCongestion;
                response.retry_after_ms = 500;
                reply(response);
                log_.info("rejected imsi=", request.imsi, ": cell full");
                return;
            }
            ue_id = next_ue_id_++;
            contexts_[ue_id] = UeContext{request.imsi, config_.cell_id, 0};
        }

        response.result = session::AttachResult::kAccepted;
        response.ue_id = ue_id;
        response.serving_cell_id = contexts_[ue_id].serving_cell;
        reply(response);
        log_.info("accepted imsi=", request.imsi, " as ue_id=", ue_id, " (", contexts_.size(), " attached)");
    }

    void on_report(const mobility::MeasurementReport& report) {
        auto it = contexts_.find(report.ue_id);
        if (it == contexts_.end()) {
            log_.info("measurement report from unknown ue_id=", report.ue_id, ", ignored");
            return;
        }
        UeContext& ue = it->second;

        const mobility::CellMeasurement* best = nullptr;
        for (const auto& neighbour : report.neighbours) {
            if (best == nullptr || neighbour.rsrp_dbm_x10 > best->rsrp_dbm_x10) best = &neighbour;
        }

        log_.info("report ue_id=", report.ue_id, " serving cell ", report.serving.cell_id, " at ",
                  report.serving.rsrp_dbm_x10 / 10.0, " dBm");

        if (best == nullptr) return;
        // int instead of int16_t: the difference of two int16 values can
        // overflow int16 in theory.
        const int margin = best->rsrp_dbm_x10 - report.serving.rsrp_dbm_x10;
        if (margin < config_.handover_margin_db_x10) return;

        mobility::HandoverCommand command;
        command.ue_id = report.ue_id;
        command.target_cell_id = best->cell_id;
        command.margin_db_x10 = static_cast<std::int16_t>(margin);
        reply(command);

        ue.serving_cell = best->cell_id;
        ue.handovers++;
        log_.info("handover ue_id=", report.ue_id, " from cell ", report.serving.cell_id, " to cell ", best->cell_id,
                  " (neighbour stronger by ", margin / 10.0, " dB)");
    }

    void on_detach(const session::DetachRequest& request) {
        // Detach is answered even for unknown ids. If the first accept got
        // lost, the UE retries, and by then the context is already gone.
        if (auto it = contexts_.find(request.ue_id); it != contexts_.end()) {
            log_.info("detached ue_id=", request.ue_id, " cause=", session::to_string(request.cause), " after ",
                      it->second.handovers, " handover(s)");
            contexts_.erase(it);
        }
        reply(session::DetachAccept{request.ue_id});
    }

    std::uint32_t find_by_imsi(std::uint64_t imsi) const {
        for (const auto& [id, context] : contexts_) {
            if (context.imsi == imsi) return id;
        }
        return 0;
    }

    template <sigflow::Message T>
    void reply(const T& message) {
        if (auto status = endpoint_.send(message); !status) {
            log_.info("failed to send ", T::kName, ": ", status.error().message());
        }
    }

    Config config_;
    sigflow::Endpoint& endpoint_;
    const sim::Logger& log_;
    std::map<std::uint32_t, UeContext> contexts_;
    std::uint32_t next_ue_id_ = 1;  // 0 means "none" in find_by_imsi
};

void print_usage() {
    std::cout << "usage: sigflow-enb [--port N] [--cell-id N] [--plmn MCCMNC] [--max-ues N]\n"
                 "                   [--handover-margin-db-x10 N]\n"
                 "Use --port 0 to pick a free port, the chosen one is printed on startup.\n";
}

int run(int argc, char** argv) {
    sim::Args args(argc, argv, {"port", "cell-id", "plmn", "max-ues", "handover-margin-db-x10"});
    if (args.help()) {
        print_usage();
        return 0;
    }

    Config config;
    config.port = args.get<std::uint16_t>("port", config.port);
    config.cell_id = args.get<std::uint32_t>("cell-id", config.cell_id);
    config.plmn = args.get<std::uint32_t>("plmn", config.plmn);
    config.max_ues = args.get<std::size_t>("max-ues", config.max_ues);
    config.handover_margin_db_x10 = args.get<std::int16_t>("handover-margin-db-x10", config.handover_margin_db_x10);

    sim::Logger log("enb");
    sim::install_stop_handler();

    auto transport = sigflow::UdpTransport::bind(config.port);
    if (!transport) {
        log.info("cannot start: ", transport.error().message());
        return 1;
    }

    sigflow::Endpoint endpoint(**transport);
    BaseStation station(config, endpoint, log);
    // The system tests wait for this exact line before starting a UE.
    log.info("ready on port ", (*transport)->local_port(), " cell ", config.cell_id);

    while (!sim::stop_requested.load()) {
        if (auto polled = endpoint.poll(200ms); !polled) {
            log.info("transport failed: ", polled.error().message());
            return 1;
        }
    }

    const auto& stats = endpoint.stats();
    log.info("shutting down, frames in=", stats.frames_received, " out=", stats.frames_sent,
             " decode errors=", stats.decode_errors, " rejected datagrams=", (*transport)->rejected_datagrams());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "sigflow-enb: " << error.what() << "\n";
        return 2;
    }
}
