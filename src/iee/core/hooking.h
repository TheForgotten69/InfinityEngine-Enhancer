#pragma once
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <MinHook.h>

namespace iee::core {
    class HookInit {
    public:
        HookInit() {
            std::lock_guard lock(state_mutex());
            auto &count = ref_count();
            if (count++ == 0) {
                const auto s = MH_Initialize();
                if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
                    count = 0;
                    throw std::runtime_error("MinHook initialization failed");
                }
            }
        }

        ~HookInit() {
            std::lock_guard lock(state_mutex());
            auto &count = ref_count();
            if (count == 0) return;
            if (--count == 0) {
                MH_Uninitialize();
            }
        }

        HookInit(const HookInit &) = delete;

        HookInit &operator=(const HookInit &) = delete;

    private:
        static std::mutex &state_mutex() {
            static std::mutex mutex;
            return mutex;
        }

        static int &ref_count() {
            static int count = 0;
            return count;
        }
    };

    template<class T>
    class Hook {
        static_assert(std::is_pointer_v<T>, "T must be a function pointer");

    public:
        Hook() = default;

        Hook(void *target, void *detour) { create(target, detour); }

        void create(void *target, void *detour) {
            if (created_) return;
            const auto st = MH_CreateHook(target, detour, reinterpret_cast<void **>(&original_));
            if (st != MH_OK) throw std::runtime_error("MH_CreateHook failed");
            target_ = target;
            created_ = true;
        }

        void enable() {
            if (!created_ || enabled_) return;
            const auto st = MH_EnableHook(target_);
            if (st != MH_OK) throw std::runtime_error("MH_EnableHook failed");
            enabled_ = true;
        }

        void disable() noexcept {
            if (!created_ || !enabled_) return;
            MH_DisableHook(target_);
            enabled_ = false;
        }

        ~Hook() { disable(); }

        T original() const noexcept { return original_; }

        Hook(const Hook &) = delete;

        Hook &operator=(const Hook &) = delete;

        Hook(Hook &&o) noexcept { *this = std::move(o); }

        Hook &operator=(Hook &&o) noexcept {
            if (this == &o) return *this;
            disable();
            target_ = o.target_;
            o.target_ = nullptr;
            original_ = o.original_;
            o.original_ = nullptr;
            created_ = o.created_;
            o.created_ = false;
            enabled_ = o.enabled_;
            o.enabled_ = false;
            return *this;
        }

    private:
        void *target_ = nullptr;
        T original_ = nullptr;
        bool created_ = false;
        bool enabled_ = false;
    };
}
