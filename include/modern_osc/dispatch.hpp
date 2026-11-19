#pragma once

#include "modern_osc/pattern.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>

namespace modern_osc {

struct NoContext {};
enum class BundlePolicy { immediate };
using HandlerId = std::uint64_t;

struct DispatchLimits {
    // A fixed cap on work across a packet, including wildcard fan-out.
    std::size_t steps = 1'000'000;
    std::size_t calls = 65'536;
    std::size_t handlers = 4096;
    PatternLimits pattern;
};

struct DispatchReport {
    std::size_t calls = 0;
    std::size_t unmatched = 0;
    std::size_t opaque = 0;
};

// Share a dispatcher on one serial execution context. Context and message
// views are borrowed for each callback. Callback exceptions propagate.
// Wildcard fan-out visits addresses lexically and handlers at each address
// in registration order. Bundle messages retain their wire order.
template<class Context = NoContext>
class Dispatcher {
public:
    using Handler = std::function<void(const MessageView&, const Context&)>;

    explicit Dispatcher(DispatchLimits limits = {}) : limits_(limits) {}

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    Result<HandlerId> add(std::string_view address, Handler handler) {
        if (delivering_) return Error{ErrorCode::busy};
        if (address.size() > limits_.pattern.bytes || handler_count_ >= limits_.handlers ||
            next_id_ == (std::numeric_limits<HandlerId>::max)())
            return Error{ErrorCode::size_limit};
        if (!is_address(address)) return Error{ErrorCode::invalid_address};
        if (!handler) return Error{ErrorCode::invalid_argument};
        const auto existing = entries_.find(address);
        if (existing != entries_.end()) {
            existing->second.push_back({next_id_, std::move(handler)});
        } else {
            std::vector<Entry> group;
            group.push_back({next_id_, std::move(handler)});
            entries_.emplace(std::string(address), std::move(group));
        }
        ++handler_count_;
        return next_id_++;
    }

    Result<bool> remove(HandlerId id) {
        if (delivering_) return Error{ErrorCode::busy};
        for (auto address = entries_.begin(); address != entries_.end(); ++address) {
            auto& group = address->second;
            const auto entry = std::find_if(group.begin(), group.end(),
                                            [id](const Entry& item) { return item.id == id; });
            if (entry == group.end()) continue;
            group.erase(entry);
            if (group.empty()) entries_.erase(address);
            --handler_count_;
            return true;
        }
        return false;
    }

    Result<DispatchReport> dispatch(const MessageView& message, const Context& context) {
        if (delivering_) return Error{ErrorCode::busy};
        Plan plan{limits_.steps};
        if (auto issue = collect_message(message, plan)) return *issue;
        return deliver(plan, context);
    }

    Result<DispatchReport> dispatch_packet(const PacketView& packet, const Context& context,
                                           BundlePolicy policy) {
        if (delivering_) return Error{ErrorCode::busy};
        if (policy != BundlePolicy::immediate) return Error{ErrorCode::invalid_argument};
        Plan plan{limits_.steps};
        if (auto issue = collect_packet(packet, plan, 0)) return *issue;
        return deliver(plan, context);
    }

private:
    struct Entry {
        HandlerId id;
        Handler handler;
    };

    struct Call {
        const MessageView* message;
        const Handler* handler;
    };

    struct Plan {
        explicit Plan(std::size_t limit) : steps_left(limit) {}
        std::size_t steps_left;
        std::vector<Call> calls;
        DispatchReport report;

        bool charge(std::size_t count) {
            if (count > steps_left) return false;
            steps_left -= count;
            return true;
        }
    };

    DispatchLimits limits_;
    std::map<std::string, std::vector<Entry>, std::less<>> entries_;
    std::size_t handler_count_ = 0;
    HandlerId next_id_ = 1;
    bool delivering_ = false;

    std::optional<Error> collect_message(const MessageView& message, Plan& plan) const {
        if (message.address.size() > limits_.pattern.bytes) return Error{ErrorCode::size_limit};
        if (!plan.charge(message.address.size()) || !plan.charge(1))
            return Error{ErrorCode::dispatch_limit};

        if (is_address(message.address)) {
            const auto found = entries_.find(message.address);
            if (found != entries_.end()) return add_calls(message, found->second, plan);
            ++plan.report.unmatched;
            return {};
        }

        auto pattern = AddressPattern::compile(message.address, limits_.pattern);
        if (!pattern) return pattern.error();
        const auto previous = plan.calls.size();
        for (const auto& [address, group] : entries_) {
            if (!plan.charge(1)) return Error{ErrorCode::dispatch_limit};
            auto matched = pattern->match(address, plan.steps_left);
            if (!matched) return matched.error();
            if (!*matched) continue;
            if (auto error = add_calls(message, group, plan)) return error;
        }
        if (plan.calls.size() == previous) ++plan.report.unmatched;
        return {};
    }

    std::optional<Error> add_calls(const MessageView& message, const std::vector<Entry>& group,
                                   Plan& plan) const {
        if (!plan.charge(group.size()) || group.size() > limits_.calls - plan.calls.size())
            return Error{ErrorCode::dispatch_limit};
        for (const auto& entry : group)
            plan.calls.push_back({&message, &entry.handler});
        return {};
    }

    std::optional<Error> collect_packet(const PacketView& packet, Plan& plan, std::size_t depth) const {
        if (depth > 64) return Error{ErrorCode::depth_limit};
        if (!plan.charge(1)) return Error{ErrorCode::dispatch_limit};
        if (const auto* message = std::get_if<MessageView>(&packet.value)) {
            return collect_message(*message, plan);
        }
        if (const auto* bundle = std::get_if<BundleView>(&packet.value)) {
            for (const auto& element : bundle->elements)
                if (auto issue = collect_packet(element, plan, depth + 1)) return issue;
        } else if (std::holds_alternative<OpaqueMessageView>(packet.value)) {
            ++plan.report.opaque;
        } else {
            return Error{ErrorCode::invalid_argument};
        }
        return {};
    }

    DispatchReport deliver(Plan& plan, const Context& context) {
        struct ResetFlag {
            bool& flag;
            ~ResetFlag() { flag = false; }
        };
        delivering_ = true;
        ResetFlag reset{delivering_};
        for (const auto& call : plan.calls) (*call.handler)(*call.message, context);
        plan.report.calls = plan.calls.size();
        return plan.report;
    }
};

} // namespace modern_osc
