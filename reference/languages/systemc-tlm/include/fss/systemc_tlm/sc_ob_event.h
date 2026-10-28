/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sc_ob_event.h — an "observable" event: it fires only when the simulator
 * ARRIVES at the notification time through its own activity (or there is later
 * activity that carries it there), and — unlike a plain sc_event::notify(delay),
 * which inserts a timed node and thereby FORCES the kernel to advance to that
 * time — it never drags time forward on its own past the point where real
 * activity would carry it.
 *
 * That is exactly what an UPPER time bound needs: arm it at t_upper and it fires
 * iff the kernel reaches t_upper, letting you stop there WITHOUT having scheduled
 * any work up to t_upper.  See protocols/time-sync.
 *
 * Original author: Mark Burton.  This is the `gs::observer_event` implementation
 * vendored VERBATIM from qbox (systemc-components/common/include/observer_event.h,
 * BSD-3-Clause, Qualcomm Innovation Center) — only the SCP report logging
 * (SCP_LOGGER/SCP_TRACE, scp/report.h) is removed so it depends ONLY on upstream
 * Accellera SystemC API, and it is exposed under the historical `sc_core::sc_ob_event`
 * name (a typedef at the end) so sc_sync_window.h is unchanged.
 *
 * Why this version and not the older sc_ob_event: the older one registered a
 * single SC_POST_UPDATE stage callback and relied on a psystemc kernel change
 * (a pre-timestep callback fired BEFORE m_curr_time advances) to insert the
 * forcing node at the bound in time.  Against a stock Accellera kernel that hook
 * is absent, so when the kernel's next activity lay PAST the bound it time-jumped
 * over it and the event fired late (the bound overshot by up to one straddling
 * wait).  This version actively RE-EVALUATES at each pending-activity step
 * (update_m): it inserts the forcing node the moment pending activity reaches the
 * bound, re-arms at the next earlier activity otherwise, and falls back to the
 * stage callback only when there is no future activity at all — so the bound is
 * never skipped on a stock kernel.
 */
#ifndef FSS_SYSTEMC_TLM_SC_OB_EVENT_H
#define FSS_SYSTEMC_TLM_SC_OB_EVENT_H

#include <systemc>
#include <string>

namespace gs {
class observer_event : public sc_core::sc_module, public sc_core::sc_event, public sc_core::sc_stage_callback_if
{
private:
    sc_core::sc_event m_update;
    sc_core::sc_time m_scheduled;
    sc_core::sc_process_handle m_th;

    void update_m()
    {
        auto now = sc_core::sc_time_stamp();
        if (now >= m_scheduled) {
            sc_core::sc_event::notify();
        } else {
            sc_core::sc_time pending = now;
            if (sc_core::sc_pending_activity_at_future_time()) {
                pending = now + sc_core::sc_time_to_pending_activity();
            }
            if (pending >= m_scheduled) {
                sc_core::sc_event::notify(m_scheduled - now);
            } else {
                if (pending > now) {
                    m_update.notify(pending - now);
                } else {
                    sc_core::sc_register_stage_callback(*this, sc_core::SC_POST_UPDATE);
                }
            }
        }
    }

    void future_events_notify_th()
    {
        while (true) {
            m_th.suspend();
            update_m();
        }
    }

public:
    observer_event(const sc_core::sc_module_name& n = sc_core::sc_gen_unique_name("observer_event"))
        : sc_module(n)
        , sc_core::sc_event((std::string(n) + "_notifier_event").c_str())
        , m_update((std::string(n) + "_update_event").c_str())
        , m_scheduled(sc_core::SC_ZERO_TIME)
    {
        SC_METHOD(update_m);
        sensitive << m_update;
        dont_initialize();
        SC_THREAD(future_events_notify_th);
        m_th = sc_core::sc_get_current_process_handle();
    }
    void notify()
    {
        m_scheduled = sc_core::sc_time_stamp();
        m_update.notify(sc_core::SC_ZERO_TIME);
    }
    void notify(double delay, sc_core::sc_time_unit unit)
    {
        m_scheduled = sc_core::sc_time_stamp() + sc_core::sc_time(delay, unit);
        m_update.notify(sc_core::SC_ZERO_TIME);
    }
    void notify(sc_core::sc_time delay)
    {
        m_scheduled = sc_core::sc_time_stamp() + delay;
        m_update.notify(sc_core::SC_ZERO_TIME);
    }

    void stage_callback(const sc_core::sc_stage& stage)
    {
        if (sc_core::sc_pending_activity_at_future_time()) {
            m_th.resume();
            sc_core::sc_unregister_stage_callback(*this, sc_core::SC_POST_UPDATE);
        }
    }

    ~observer_event() {}
};
} // namespace gs

namespace sc_core {
/* Historical name used by sc_sync_window.h (and any FSS code): the observable
 * bound event IS gs::observer_event. */
typedef gs::observer_event sc_ob_event;
} // namespace sc_core

#endif // FSS_SYSTEMC_TLM_SC_OB_EVENT_H
