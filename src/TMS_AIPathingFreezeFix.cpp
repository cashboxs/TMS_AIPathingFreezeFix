#include "PCH.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace
{
    constexpr auto kVersion = "1.1";
    constexpr std::size_t kCallSize = 5;
    constexpr std::size_t kAngleValidationWindow = 0xA0;
    constexpr std::uint64_t kDetailedRepairLimit = 3;
    constexpr ULONGLONG kSummaryIntervalMs = 60'000;

    struct Float3
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    static_assert(sizeof(Float3) == sizeof(float) * 3);
    static_assert(std::numeric_limits<float>::is_iec559);

    using ProducerFn = bool (*)(void*, void*);

    std::atomic<std::uint64_t> g_repairCount{ 0 };
    std::atomic<std::uint64_t> g_lastSummaryRepairCount{ kDetailedRepairLimit };
    std::atomic<ULONGLONG> g_lastSummaryTick{ 0 };

    bool IsFiniteFloat(float value) noexcept
    {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        return (bits & 0x7F800000u) != 0x7F800000u;
    }

    bool InRange(std::uintptr_t address, std::uintptr_t begin, std::size_t size) noexcept
    {
        return address >= begin && address - begin < size;
    }

    std::uintptr_t ResolveCallTarget(std::uintptr_t callsite) noexcept
    {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(callsite + 1), sizeof(displacement));
        return static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(callsite + kCallSize) + static_cast<std::intptr_t>(displacement));
    }

    bool HasAngleLoop(std::uintptr_t function, std::uintptr_t textBegin, std::size_t textSize) noexcept
    {
        static constexpr std::array<std::uint8_t, 9> pattern{
            0xF3, 0x0F, 0x58, 0xC6, 0x0F, 0x2F, 0xC7, 0x72, 0xF7
        };

        if (!InRange(function, textBegin, textSize)) {
            return false;
        }

        const auto available = textSize - static_cast<std::size_t>(function - textBegin);
        const auto window = (std::min)(available, kAngleValidationWindow);
        if (window < pattern.size()) {
            return false;
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(function);
        for (std::size_t i = 0; i + pattern.size() <= window; ++i) {
            if (std::memcmp(bytes + i, pattern.data(), pattern.size()) == 0) {
                return true;
            }
        }

        return false;
    }

    bool HasRipRelativeMovss(std::uintptr_t begin, std::uintptr_t end) noexcept
    {
        if (end <= begin || end - begin < 8) {
            return false;
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(begin);
        const auto size = static_cast<std::size_t>(end - begin);
        for (std::size_t i = 0; i + 8 <= size; ++i) {
            if (bytes[i] == 0xF3 &&
                bytes[i + 1] == 0x0F &&
                bytes[i + 2] == 0x10 &&
                (bytes[i + 3] & 0xC7) == 0x05) {
                return true;
            }
        }

        return false;
    }

    // AE pattern (1.6.x): lea rdx,[rbp+XX] / lea rcx,[rsp+..] before producer,
    // lea rcx,[rbp+XX] before angle. SE 1.5.97 uses rsp-relative locals instead.
    bool MatchProducerCallAE(
        std::uintptr_t angleCall,
        std::uintptr_t textBegin,
        std::size_t textSize,
        std::uintptr_t& producerCall) noexcept
    {
        producerCall = 0;

        if (!InRange(angleCall, textBegin, textSize) || angleCall < textBegin + 40) {
            return false;
        }

        const auto* angle = reinterpret_cast<const std::uint8_t*>(angleCall);
        if (angle[-4] != 0x48 || angle[-3] != 0x8D || angle[-2] != 0x4D) {
            return false;
        }

        const auto localDisplacement = angle[-1];
        const auto searchBegin = angleCall - 40;
        const auto searchEnd = angleCall - 9;

        for (auto candidate = searchBegin; candidate <= searchEnd; ++candidate) {
            const auto* call = reinterpret_cast<const std::uint8_t*>(candidate);
            if (*call != 0xE8 || candidate < textBegin + 9) {
                continue;
            }

            const auto* prefix = call - 9;
            if (prefix[0] != 0x48 ||
                prefix[1] != 0x8D ||
                prefix[2] != 0x55 ||
                prefix[4] != 0x48 ||
                prefix[5] != 0x8D ||
                prefix[6] != 0x4C ||
                prefix[7] != 0x24 ||
                prefix[3] != localDisplacement) {
                continue;
            }

            const auto middleBegin = candidate + kCallSize;
            const auto middleEnd = angleCall - 4;
            if (middleEnd <= middleBegin || middleEnd - middleBegin > 24) {
                continue;
            }

            if (!HasRipRelativeMovss(middleBegin, middleEnd)) {
                continue;
            }

            producerCall = candidate;
            return true;
        }

        return false;
    }

    // ponytail: SE 1.5.97 pattern only, add when another runtime needs its own shape.
    // Verified on decrypted 1.5.97: producer callsite SkyrimSE.exe+0x10BDEAD,
    // angle callsite SkyrimSE.exe+0x10BDEBF, angle SkyrimSE.exe+0xC51F70 (REL ID 68820).
    // Sequence: lea rdx,[rsp+XX] / mov rcx,rax / call producer /
    // movss xmm,[RIP] / lea rcx,[rsp+XX] / call angle. Return value ignored by caller.
    bool MatchProducerCallSE(
        std::uintptr_t angleCall,
        std::uintptr_t textBegin,
        std::size_t textSize,
        std::uintptr_t& producerCall) noexcept
    {
        producerCall = 0;

        if (!InRange(angleCall, textBegin, textSize) || angleCall < textBegin + 40) {
            return false;
        }

        const auto* angle = reinterpret_cast<const std::uint8_t*>(angleCall);
        if (angle[-5] != 0x48 || angle[-4] != 0x8D || angle[-3] != 0x4C || angle[-2] != 0x24) {
            return false;
        }

        const auto rspDisplacement = angle[-1];
        const auto searchBegin = angleCall - 40;
        const auto searchEnd = angleCall - 9;

        for (auto candidate = searchBegin; candidate <= searchEnd; ++candidate) {
            const auto* call = reinterpret_cast<const std::uint8_t*>(candidate);
            if (*call != 0xE8 || candidate < textBegin + 8) {
                continue;
            }

            // lea rdx,[rsp+YY] with YY == angle's rsp displacement,
            // followed by mov rcx,rax (source pointer forwarded from entry rdx).
            const auto* prefix = call - 8;
            if (prefix[0] != 0x48 || prefix[1] != 0x8D || prefix[2] != 0x54 || prefix[3] != 0x24 ||
                prefix[4] != rspDisplacement || prefix[5] != 0x48 || prefix[6] != 0x8B ||
                prefix[7] != 0xC8) {
                continue;
            }

            const auto middleBegin = candidate + kCallSize;
            const auto middleEnd = angleCall - 5;
            if (middleEnd <= middleBegin || middleEnd - middleBegin > 24) {
                continue;
            }

            if (!HasRipRelativeMovss(middleBegin, middleEnd)) {
                continue;
            }

            producerCall = candidate;
            return true;
        }

        return false;
    }

    bool MatchProducerCall(
        std::uintptr_t angleCall,
        std::uintptr_t textBegin,
        std::size_t textSize,
        std::uintptr_t& producerCall) noexcept
    {
        if (MatchProducerCallAE(angleCall, textBegin, textSize, producerCall)) {
            return true;
        }
        return MatchProducerCallSE(angleCall, textBegin, textSize, producerCall);
    }

    std::uintptr_t FindVulnerableCallsite(
        std::uintptr_t angleFunction,
        std::uintptr_t textBegin,
        std::size_t textSize,
        std::size_t& matchCount) noexcept
    {
        matchCount = 0;
        std::uintptr_t result = 0;

        if (!textBegin || textSize < kCallSize) {
            return 0;
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(textBegin);
        for (std::size_t i = 0; i + kCallSize <= textSize; ++i) {
            if (bytes[i] != 0xE8) {
                continue;
            }

            const auto angleCall = textBegin + i;
            if (ResolveCallTarget(angleCall) != angleFunction) {
                continue;
            }

            std::uintptr_t producerCall = 0;
            if (!MatchProducerCall(angleCall, textBegin, textSize, producerCall)) {
                continue;
            }

            ++matchCount;
            result = producerCall;
            i += kCallSize - 1;
        }

        return matchCount == 1 ? result : 0;
    }

    bool IsExecutableAddress(std::uintptr_t address) noexcept
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || ::VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
            return false;
        }

        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0) {
            return false;
        }

        const auto protect = info.Protect & 0xFF;
        return protect == PAGE_EXECUTE ||
               protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE ||
               protect == PAGE_EXECUTE_WRITECOPY;
    }

    void LogRepair(std::uint64_t repairCount) noexcept
    {
        if (repairCount <= kDetailedRepairLimit) {
            try {
                spdlog::warn(
                    "AI FREEZE FIX repair={} replacement=[0,0,0]",
                    repairCount);
            } catch (...) {
            }
            return;
        }

        const auto now = ::GetTickCount64();
        auto last = g_lastSummaryTick.load(std::memory_order_relaxed);
        if (now - last < kSummaryIntervalMs) {
            return;
        }

        if (!g_lastSummaryTick.compare_exchange_strong(
                last,
                now,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }

        const auto total = g_repairCount.load(std::memory_order_relaxed);
        const auto previous = g_lastSummaryRepairCount.exchange(total, std::memory_order_relaxed);
        if (total <= previous) {
            return;
        }

        try {
            spdlog::info(
                "AI FREEZE FIX summary totalRepairs={} repairsLastInterval={}",
                total,
                total - previous);
        } catch (...) {
        }
    }

    struct FixHook
    {
        static bool Thunk(void* source, void* output) noexcept
        {
            const bool result = Original(source, output);
            if (result || !output) {
                return result;
            }

            Float3 value{};
            std::memcpy(&value, output, sizeof(value));
            if (IsFiniteFloat(value.x) && IsFiniteFloat(value.y)) {
                return result;
            }

            const Float3 safe{};
            std::memcpy(output, &safe, sizeof(safe));
            const auto repairCount = g_repairCount.fetch_add(1, std::memory_order_relaxed) + 1;
            LogRepair(repairCount);
            return result;
        }

        static bool Install()
        {
            if (REL::Module::IsVR()) {
                spdlog::critical("AI Pathing Freeze Fix v{} does not support VR.", kVersion);
                return false;
            }

            static REL::Relocation<std::uintptr_t> angleFunction{ REL::RelocationID(68820, 70172) };
            const auto angleAddress = angleFunction.address();
            const auto& module = REL::Module::get();
            const auto text = module.segment(REL::Segment::textx);
            const auto textBegin = text.address();
            const auto textSize = text.size();

            if (!textBegin || textSize < kCallSize || !angleAddress) {
                spdlog::critical(
                    "AI Pathing Freeze Fix v{} install failed: invalid text segment or angle relocation.",
                    kVersion);
                return false;
            }

            if (!HasAngleLoop(angleAddress, textBegin, textSize)) {
                spdlog::critical(
                    "AI Pathing Freeze Fix v{} install refused: angle routine validation failed for runtime {}.",
                    kVersion,
                    module.version().string());
                return false;
            }

            std::size_t matchCount = 0;
            const auto callsite = FindVulnerableCallsite(angleAddress, textBegin, textSize, matchCount);
            if (!callsite) {
                spdlog::critical(
                    "AI Pathing Freeze Fix v{} install refused: vulnerable callsite matches={} runtime={}.",
                    kVersion,
                    matchCount,
                    module.version().string());
                return false;
            }

            const auto originalTarget = ResolveCallTarget(callsite);
            if (originalTarget == angleAddress || !IsExecutableAddress(originalTarget)) {
                spdlog::critical(
                    "AI Pathing Freeze Fix v{} install refused: original call target is not executable.",
                    kVersion);
                return false;
            }

            Original = reinterpret_cast<ProducerFn>(originalTarget);
            g_lastSummaryTick.store(::GetTickCount64(), std::memory_order_relaxed);
            SKSE::AllocTrampoline(14);
            auto& trampoline = SKSE::GetTrampoline();
            trampoline.write_call<5>(callsite, Thunk);

            spdlog::info(
                "AI Pathing Freeze Fix v{} installed runtime={} kind={} callsite=SkyrimSE.exe+0x{:X} angle=SkyrimSE.exe+0x{:X}.",
                kVersion,
                module.version().string(),
                REL::Module::IsAE() ? "AE" : "SE",
                callsite - module.base(),
                angleAddress - module.base());
            return true;
        }

        static inline ProducerFn Original{ nullptr };
    };

    void SetupLog() noexcept
    {
        try {
            auto logDir = SKSE::log::log_directory();
            if (!logDir) {
                return;
            }

            auto path = *logDir;
            path /= "TMS_AIPathingFreezeFix.log";
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("global log", std::move(sink));
            spdlog::set_default_logger(std::move(logger));
            spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::warn);
        } catch (...) {
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();

    const auto& module = REL::Module::get();
    spdlog::info(
        "AI Pathing Freeze Fix v{} loading runtime={} kind={}.",
        kVersion,
        module.version().string(),
        REL::Module::IsVR() ? "VR" : (REL::Module::IsAE() ? "AE" : "SE"));

    if (!FixHook::Install()) {
        spdlog::critical("AI Pathing Freeze Fix v{} NOT armed.", kVersion);
        return false;
    }

    return true;
}
