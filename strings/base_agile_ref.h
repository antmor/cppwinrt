
WINRT_EXPORT namespace winrt
{
#if defined (WINRT_NO_MODULE_LOCK)

    // Defining WINRT_NO_MODULE_LOCK is appropriate for apps (executables) or pinned DLLs (that don't support unloading)
    // and can thus avoid the synchronization overhead imposed by the default module lock.

    constexpr auto get_module_lock() noexcept
    {
        struct lock
        {
            constexpr uint32_t operator++() noexcept
            {
                return 1;
            }

            constexpr uint32_t operator--() noexcept
            {
                return 0;
            }

            constexpr explicit operator bool() noexcept
            {
                return true;
            }
        };

        return lock{};
    }

#elif defined (WINRT_CUSTOM_MODULE_LOCK)

    // When WINRT_CUSTOM_MODULE_LOCK is defined, you must provide an implementation of winrt::get_module_lock()
    // that returns an object that implements operator++ and operator--.

#else

    // This is the default implementation for use with DllCanUnloadNow.

    inline impl::atomic_ref_count& get_module_lock() noexcept
    {
        static impl::atomic_ref_count s_lock;
        return s_lock;
    }

#endif
}

namespace winrt::impl
{
    template<bool UseModuleLock>
    struct module_lock_updater;

    template<>
    struct module_lock_updater<true>
    {
        module_lock_updater() noexcept
        {
            ++get_module_lock();
        }

        ~module_lock_updater() noexcept
        {
            --get_module_lock();
        }
    };

    template<>
    struct module_lock_updater<false> {};

    using update_module_lock = module_lock_updater<true>;

    struct agile_ref_fallback final : IAgileReference, update_module_lock
    {
        agile_ref_fallback(com_ptr<IGlobalInterfaceTable>&& git, uint32_t cookie) noexcept :
            m_git(std::move(git)),
            m_cookie(cookie)
        {
        }

        ~agile_ref_fallback() noexcept
        {
            m_git->RevokeInterfaceFromGlobal(m_cookie);
        }

        int32_t __stdcall QueryInterface(guid const& id, void** object) noexcept final
        {
            if (is_guid_of<IAgileReference>(id) || is_guid_of<Windows::Foundation::IUnknown>(id) || is_guid_of<IAgileObject>(id))
            {
                *object = static_cast<IAgileReference*>(this);
                AddRef();
                return 0;
            }

            *object = nullptr;
            return error_no_interface;
        }

        uint32_t __stdcall AddRef() noexcept final
        {
            return ++m_references;
        }

        uint32_t __stdcall Release() noexcept final
        {
            auto const remaining = --m_references;

            if (remaining == 0)
            {
                delete this;
            }

            return remaining;
        }

        int32_t __stdcall Resolve(guid const& id, void** object) noexcept final
        {
            return m_git->GetInterfaceFromGlobal(m_cookie, id, object);
        }

    private:

        com_ptr<IGlobalInterfaceTable> m_git;
        uint32_t m_cookie{};
        atomic_ref_count m_references{ 1 };
    };

    inline void* load_library(wchar_t const* library) noexcept
    {
        return WINRT_IMPL_LoadLibraryExW(library, nullptr, 0x00001000 /* LOAD_LIBRARY_SEARCH_DEFAULT_DIRS */);
    }

    inline hresult get_agile_reference(winrt::guid const& iid, void* object, void** reference) noexcept
    {
        return WINRT_IMPL_RoGetAgileReference(0, iid, object, reference);
    }
}

WINRT_EXPORT namespace winrt
{
    template <typename T>
    struct agile_ref
    {
        agile_ref(std::nullptr_t = nullptr) noexcept {}

        agile_ref(impl::com_ref<T> const& object)
        {
            if (object)
            {
                check_hresult(impl::get_agile_reference(guid_of<T>(), winrt::get_abi(object), m_ref.put_void()));
            }
        }

        [[nodiscard]] impl::com_ref<T> get() const noexcept
        {
            if (!m_ref)
            {
                return nullptr;
            }

            void* result{};
            m_ref->Resolve(guid_of<T>(), &result);
            return { result, take_ownership_from_abi };
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(m_ref);
        }

    private:

        com_ptr<impl::IAgileReference> m_ref;
    };

    template<typename T> agile_ref(T const&)->agile_ref<impl::wrapped_type_t<T>>;

    template <typename T>
    agile_ref<impl::wrapped_type_t<T>> make_agile(T const& object)
    {
        return object;
    }
}
