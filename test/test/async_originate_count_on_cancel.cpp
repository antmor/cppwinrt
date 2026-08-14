#include "pch.h"

using namespace winrt;
using namespace Windows::Foundation;

namespace
{
    //
    // Checks that cancellation_token::originate_on_cancel(false) suppresses origination
    // on every path that can throw hresult_canceled, and that it stays suppressed for the
    // lifetime of the coroutine rather than just for the first cancellation check.
    //
    // NOTE: these tests do not observe RoOriginateLanguageException itself. They count
    // calls to winrt_throw_hresult_handler as a *proxy* for it: hresult_error::originate
    // invokes the handler and calls RoOriginateLanguageException together, so the handler
    // firing means origination was attempted on that path. The two are not equivalent --
    // RoOriginateLanguageException does not raise the debugger notification on every call,
    // so a handler count is an upper bound on the notifications a debugger would see, and
    // the handler is also reachable for non-cancellation errors. Only ERROR_CANCELLED
    // results are counted here, which is what originate_on_cancel controls. Observing the
    // RoOriginateLanguageException calls themselves requires an out-of-process debugger
    // and is out of scope for a unit test.
    //

    std::atomic<int> s_originations{ 0 };

    void __stdcall countOriginations(uint32_t, char const*, char const*, void*, winrt::hresult const result) noexcept
    {
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            s_originations.fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct origination_counter
    {
        origination_counter()
        {
            REQUIRE(!winrt_throw_hresult_handler);
            s_originations.store(0, std::memory_order_relaxed);
            winrt_throw_hresult_handler = countOriginations;
        }

        ~origination_counter()
        {
            winrt_throw_hresult_handler = nullptr;
        }

        origination_counter(origination_counter const&) = delete;
        void operator=(origination_counter const&) = delete;

        int count() const noexcept
        {
            return s_originations.load(std::memory_order_relaxed);
        }
    };

    // Never cancelled, so nothing should ever originate. This is the zero baseline that
    // shows the counter is not picking up originations from the surrounding machinery.
    IAsyncAction NoCancelAction(HANDLE resume, bool originate)
    {
        auto cancel = co_await get_cancellation_token();
        cancel.originate_on_cancel(originate);

        co_await resume_on_signal(resume);
        co_return;
    }

    // Cancelled while suspended, then resumed and allowed to return without reaching
    // another co_await, so the only cancellation check is in promise_base::Cancel.
    IAsyncAction SingleCheckAction(HANDLE resume, bool originate)
    {
        auto cancel = co_await get_cancellation_token();
        cancel.originate_on_cancel(originate);

        co_await resume_on_signal(resume);

        REQUIRE(cancel());
        co_return;
    }

    // Same as above, but reaches a further co_await after being cancelled so that
    // promise_base::await_transform performs a second cancellation check. This is the
    // case a single-cancellation test cannot catch: the guard used to re-arm origination
    // after the first check, so only the second and later checks regressed.
    IAsyncAction RepeatedChecksAction(HANDLE resume, bool originate)
    {
        auto cancel = co_await get_cancellation_token();
        cancel.originate_on_cancel(originate);

        co_await resume_on_signal(resume);

        // Status() is Canceled by now, so this co_await throws from await_transform.
        co_await std::suspend_never();
        REQUIRE(false);
    }

    // Cancellation is propagated into an awaited WinRT async, so the outer coroutine
    // resumes through await_adapter::await_resume -> check_status_canceled.
    IAsyncAction InnerSignalAction(HANDLE started, bool originate)
    {
        co_await resume_background();

        auto cancel = co_await get_cancellation_token();
        cancel.enable_propagation();
        cancel.originate_on_cancel(originate);

        SetEvent(started);
        co_await resume_on_signal(GetCurrentProcess()); // never wakes
        REQUIRE(false);
    }

    IAsyncAction AwaitAsyncAction(HANDLE started, bool originate)
    {
        co_await resume_background();

        auto cancel = co_await get_cancellation_token();
        cancel.enable_propagation();
        cancel.originate_on_cancel(originate);

        co_await InnerSignalAction(started, originate);
        REQUIRE(false);
    }

    // Resumes through resume_after, which is timespan_awaiter::await_resume.
    IAsyncAction ResumeAfterAction(HANDLE started, bool originate)
    {
        co_await resume_background();

        auto cancel = co_await get_cancellation_token();
        cancel.enable_propagation();
        cancel.originate_on_cancel(originate);

        SetEvent(started);
        co_await resume_after(std::chrono::hours(1)); // effectively sleeps forever
        REQUIRE(false);
    }

    // Resumes through resume_on_signal, which is signal_awaiter::await_resume.
    IAsyncAction ResumeOnSignalAction(HANDLE started, bool originate)
    {
        co_await resume_background();

        auto cancel = co_await get_cancellation_token();
        cancel.enable_propagation();
        cancel.originate_on_cancel(originate);

        SetEvent(started);
        co_await resume_on_signal(GetCurrentProcess()); // never wakes
        REQUIRE(false);
    }

    void WaitForCompletion(HANDLE completed)
    {
        REQUIRE(WaitForSingleObject(completed, IsDebuggerPresent() ? INFINITE : 10000) == WAIT_OBJECT_0);
    }

    // Cancels a coroutine that is suspended on a cancellable await and returns the number
    // of cancellation originations observed while it unwound.
    template <typename F>
    int CountOriginationsOnPropagatedCancel(F make, bool originate)
    {
        handle started{ CreateEvent(nullptr, true, false, nullptr) };
        handle completed{ CreateEvent(nullptr, true, false, nullptr) };
        int originations = 0;

        {
            origination_counter counter;

            auto async = make(started.get(), originate);
            async.Completed([&](auto&&, AsyncStatus status)
                {
                    REQUIRE(status == AsyncStatus::Canceled);
                    SetEvent(completed.get());
                });

            // Make sure the coroutine has reached the cancellable await before cancelling,
            // otherwise cancellation is observed by await_transform instead and a
            // different set of origination sites runs on the way out.
            WaitForCompletion(started.get());
            Sleep(500);

            async.Cancel();
            WaitForCompletion(completed.get());

            originations = counter.count();
            REQUIRE(async.Status() == AsyncStatus::Canceled);
        }

        return originations;
    }

    // Runs a coroutine that suspends on a non-cancellable resume_on_signal, optionally
    // cancels it while it is suspended, then releases it and counts what originated.
    template <typename F>
    int CountOriginations(F make, bool originate, bool cancel)
    {
        handle resume{ CreateEvent(nullptr, true, false, nullptr) };
        handle completed{ CreateEvent(nullptr, true, false, nullptr) };
        auto const expected = cancel ? AsyncStatus::Canceled : AsyncStatus::Completed;
        int originations = 0;

        {
            origination_counter counter;

            auto async = make(resume.get(), originate);
            async.Completed([&](auto&&, AsyncStatus status)
                {
                    REQUIRE(status == expected);
                    SetEvent(completed.get());
                });

            if (cancel)
            {
                async.Cancel();
            }

            SetEvent(resume.get());
            WaitForCompletion(completed.get());

            originations = counter.count();
            REQUIRE(async.Status() == expected);
        }

        return originations;
    }

    int CountOriginationsWithoutCancel(bool originate)
    {
        return CountOriginations(NoCancelAction, originate, false);
    }

    int CountOriginationsOnSingleCheck(bool originate)
    {
        return CountOriginations(SingleCheckAction, originate, true);
    }

    int CountOriginationsOnRepeatedChecks(bool originate)
    {
        return CountOriginations(RepeatedChecksAction, originate, true);
    }
}

// NOTE: tagged [.clang-crash] for consistency with every other async cancellation test in
// this directory (async_check_cancel, async_propagate_cancel, async_auto_cancel, ...),
// which are all hidden under clang because cancelling a coroutine segfaults there. This
// test drives the same machinery, but the tag has NOT been independently verified against
// clang-cl -- see the FIXME on those tests for the underlying issue.
#if defined(__clang__) && defined(_MSC_VER)
TEST_CASE("async_originate_count_on_cancel", "[.clang-crash]")
#else
TEST_CASE("async_originate_count_on_cancel")
#endif
{
    // Baseline: no cancellation at all, so the opt-out setting is irrelevant.
    SECTION("no cancellation")
    {
        REQUIRE(CountOriginationsWithoutCancel(true) == 0);
        REQUIRE(CountOriginationsWithoutCancel(false) == 0);
    }

    // Baseline: exactly one cancellation check, in promise_base::Cancel. This passes even
    // with the setter/getter bug present, which is why the repeated-check case below is
    // the meaningful one.
    SECTION("single cancellation check")
    {
        REQUIRE(CountOriginationsOnSingleCheck(true) == 1);
        REQUIRE(CountOriginationsOnSingleCheck(false) == 0);
    }

    // promise_base::Cancel, then promise_base::await_transform on the next co_await.
    SECTION("repeated cancellation checks")
    {
        REQUIRE(CountOriginationsOnRepeatedChecks(true) == 2);
        REQUIRE(CountOriginationsOnRepeatedChecks(false) == 0);
    }

    // Outer promise_base::Cancel, inner promise_base::Cancel via propagation, the inner
    // coroutine's own signal_awaiter::await_resume, and finally the outer coroutine's
    // await_adapter::await_resume -> check_status_canceled.
    SECTION("awaited async completes canceled")
    {
        REQUIRE(CountOriginationsOnPropagatedCancel(AwaitAsyncAction, true) == 4);
        REQUIRE(CountOriginationsOnPropagatedCancel(AwaitAsyncAction, false) == 0);
    }

    // promise_base::Cancel, then timespan_awaiter::await_resume.
    SECTION("resume_after canceled")
    {
        REQUIRE(CountOriginationsOnPropagatedCancel(ResumeAfterAction, true) == 2);
        REQUIRE(CountOriginationsOnPropagatedCancel(ResumeAfterAction, false) == 0);
    }

    // promise_base::Cancel, then signal_awaiter::await_resume.
    SECTION("resume_on_signal canceled")
    {
        REQUIRE(CountOriginationsOnPropagatedCancel(ResumeOnSignalAction, true) == 2);
        REQUIRE(CountOriginationsOnPropagatedCancel(ResumeOnSignalAction, false) == 0);
    }
}
