
#pragma once
#include "types/venue.h"
#include "venue_book.h"
#include "consolidated_bbo.h"
#include "types.h"
#include "logger/logger.h"
#include <functional>
#include <mutex>

namespace market_data {

using InstrumentBooks = std::unordered_map<InstrumentId, VenueBookArray>;

class Core {
   public:
    using BboCallback = std::function<void(InstrumentId, const consolidated::BBO&)>;

    explicit Core(BboCallback bbo_callback = nullptr) : bbo_callback_(std::move(bbo_callback)) {}
    ~Core() = default;

    void Init(const CoreConfig& config);

    // Fed by whoever owns the Provider(s) (the wiring layer, e.g. main.cpp).
    // Core has no knowledge of providers, sockets, or threads.
    void ApplyUpdate(const BookUpdate& update);

    void Start() {}
    void Stop() {}

   private:
    // Creates the 3-slot VenueBookArray for `instrument`, one VenueBook per
    // venue in `venues`. Called from init() for each startup instrument; not
    // exposed as a live "subscribe" API yet (out of scope for now, see
    // DESIGN_1 §1.2 - multi-symbol is designed for, not exercised).
    void AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues);
    void RemoveInstrument(InstrumentId instrument);

    // Interim fix, not the final architecture: guards venue_books_ against
    // concurrent ApplyUpdate() calls from multiple Provider threads. The
    // designed fix (DESIGN_1 §7.3) is per-venue SPSC queues drained by one
    // consolidator thread, with no lock on the book path at all - not built
    // yet. This mutex is correct but costs contention the real design
    // wouldn't have; remove it once the SPSC queue replaces this.
    std::mutex apply_mutex_;
    BboCallback bbo_callback_;
    InstrumentBooks venue_books_;
};

}  // namespace market_data
