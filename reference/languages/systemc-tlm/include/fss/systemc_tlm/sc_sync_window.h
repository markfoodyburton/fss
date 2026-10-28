/*****************************************************************************
  Licensed to Accellera Systems Initiative Inc. (Accellera) under one or more
  contributor license agreements.  Accellera licenses this file to you under
  the Apache License, Version 2.0 (the "License"); you may not use this file
  except in compliance with the License.  You may obtain a copy of the License
  at http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
  License for the specific language governing permissions and limitations
  under the License.
 *****************************************************************************/

/*
 * sc_sync_window.h — keeps a SystemC kernel within a time window [from, to].
 *
 * This is the *protocol* around the sc_ob_event *primitive*: a foreign thread
 * delivers a window via async_set_window(); the kernel runs to `to` and no
 * further (sc_ob_event at `to` → sc_suspend_all, in-flight sc_unsuspendable
 * transactions may overshoot); on reaching `to` it reports its own next window
 * via the sync callback; a new window unsuspends and re-arms.  The class is an
 * sc_prim_channel so a foreign thread's async_request_update can wake a
 * suspended kernel (the only deadlock-safe cross-thread path — sc_suspend_all
 * merely counts; the kernel OS-blocks at starvation on the suspend semaphore,
 * which async_request_update posts; update() then runs on the kernel thread and
 * notifies step_helper to unsuspend/re-arm).  This is the mechanism behind the
 * FSS time-sync window (protocols/time-sync) on fss/time.
 *
 * Original author: Mark Burton.  Vendored from psystemc
 * (src/sysc/utils/sc_sync_window.h); depends ONLY on upstream Accellera SystemC
 * API + the vendored sc_ob_event (which is itself upstream-API-only), so FSS
 * needs no psystemc.  Kept faithful (only the sc_ob_event include path differs)
 * so it tracks the upstream proposal; the subtle sc_suspend/unsuspend balancing
 * is the reason it is vendored rather than paraphrased.
 */
#ifndef FSS_SYSTEMC_TLM_SC_SYNC_WINDOW_H
#define FSS_SYSTEMC_TLM_SC_SYNC_WINDOW_H

#include <mutex>
#include "fss/systemc_tlm/sc_ob_event.h"
#include <systemc>
#include <tlm_utils/tlm_quantumkeeper.h>

namespace sc_core {

/**
 * @brief Base class which gives the quantum (dynamically) and specifies if the
 * window should stay open indefinitely or detach when there are no more events.
 * A SC_ZERO_TIME quantum means sync on every time step
 */
struct sc_sync_policy_base {
  virtual sc_core::sc_time quantum() = 0;
  virtual bool keep_alive() = 0;
};
struct sc_sync_policy_tlm_quantum : public sc_sync_policy_base {
  sc_core::sc_time quantum() {
    return tlm_utils::tlm_quantumkeeper::get_global_quantum();
  }
  bool keep_alive() { return true; }
};
struct sc_sync_policy_in_sync : public sc_sync_policy_base {
  sc_core::sc_time quantum() {
    return sc_core::sc_pending_activity()
               ? sc_core::sc_time_to_pending_activity()
               : sc_core::SC_ZERO_TIME;
  }
  bool keep_alive() { return false; }
};
/**
 * @brief windowed synchronizer, template sc_sync_policy gives the quantum
 * (dynamically) and specifies if the window should stay open indefinitely or
 * detach when there are no more events.
 *
 * @tparam sc_sync_policy
 */
template <class sc_sync_policy = sc_sync_policy_in_sync>
class sc_sync_windowed : public sc_core::sc_module,
                         public sc_core::sc_prim_channel {
  static_assert(std::is_base_of<sc_sync_policy_base, sc_sync_policy>::value,
                "sc_sync_policy must derive from sc_sync_policy_base");

  sc_core::sc_event m_sweep_ev;
  sc_ob_event m_step_ev;
  sc_event m_update_ev;
  std::mutex m_mutex;
  sc_sync_policy policy;

public:
  struct window {
    sc_core::sc_time from;
    sc_core::sc_time to;
    bool operator==(const window &other) const {
      return other.to == to && other.from == from;
    }
  };

  static inline const struct window zero_window = {sc_core::SC_ZERO_TIME,
                                     sc_core::SC_ZERO_TIME};
  static inline const struct window open_window = {sc_core::SC_ZERO_TIME,
                                     sc_core::sc_max_time()};

private:
  window m_window;
  window m_incomming_window; // used to hold the window values coming in from
                             // the other side.

  std::function<void(const window &)> m_other_async_set_window_fn;

  void do_other_async_set_window_fn(window w) {
    if (m_other_async_set_window_fn) {
      m_other_async_set_window_fn(w);
    }
  }

  /* Handle suspending/resuming at the end of the window, also inform other side
   * if we reach end of window */
  void step_helper() {
    auto now = sc_core::sc_time_stamp();
    auto to = m_window.to;

    /* The step helper has to handle both suspend and resume (because of
     * SystemC) */
    if (now >= to) {
      sc_core::sc_unsuspend_all(); // such that pending activity is valid if
                                   // it's needed below.

      /* We should suspend at this point, and wait for the other side to catch
       * up */
      do_other_async_set_window_fn({now, now + policy.quantum()});

      if (!policy.keep_alive())
        async_attach_suspending();
      sc_core::sc_suspend_all();

    } else {
      /* the only way to get here is if we have a 'new' window from the other
       * side. we are here just to unsuspend */
      sc_core::sc_unsuspend_all();
      if (!policy.keep_alive())
        async_detach_suspending();
    //do_other_async_set_window_fn({now, now + policy.quantum()});

      /* Re-notify event - maybe presumably moved */
      m_step_ev.notify(to - now);
    }
  }

  /*
   * Handle all sweep requests, once we arrive at a sweep point, tell the other
   * side.
   */
  void sweep_helper() {
    auto now = sc_core::sc_time_stamp();
    do_other_async_set_window_fn({now, now + policy.quantum()});
  }

  /* Manage the Sync aync update  */
  void update() {
    std::lock_guard<std::mutex> lg(m_mutex);
    // Now we are on our thread, it's safe to update our window.
    m_window = m_incomming_window;
    auto now = sc_core::sc_time_stamp();

    if (m_window.from > now) {
      m_sweep_ev.notify(m_window.from - now); // Try to move time forward.
    } else {
      m_sweep_ev.cancel(); // no need to fire event.
    }
    /* let stepper handle suspend/resume, must time notify */
    m_update_ev.notify(sc_core::SC_ZERO_TIME);
  }

public:
  /* API call from other pair, presumably in a different thread.
   * The internal window wil be updated atomically.
   *
   * Input: window  - Window to set for sync. Sweep till the 'from' and step to
   * the 'to'.
   */
  void async_set_window(const window &w) {
    /* Only accept updated windows so we dont re-send redundant updates
     * safe at this point to compair against m_window as we took the lock
     */
    std::lock_guard<std::mutex> lg(m_mutex);
    m_incomming_window = w;
    if (!(w == m_window)) {
      async_request_update();
    }
  }
  void detach() {
    async_detach_suspending();
    m_other_async_set_window_fn(open_window);
  }
  void bind(sc_sync_windowed *other) {
    if (m_other_async_set_window_fn) {
      SC_REPORT_WARNING(
          "sc_sync_window",
          "m_other_async_set_window_fn was already registered or other "
          "sc_sync_windowed was already bound!");
    }
    m_other_async_set_window_fn = std::bind(&sc_sync_windowed::async_set_window, other, std::placeholders::_1);
  }
  void register_sync_cb(std::function<void(const window &)> fn) {
    if (m_other_async_set_window_fn) {
      SC_REPORT_WARNING(
          "sc_sync_window",
          "m_other_async_set_window_fn was already registered or other "
          "sc_sync_windowed was already bound!");
    }
    m_other_async_set_window_fn = fn;
  }
  SC_CTOR(sc_sync_windowed)
      : m_window({sc_core::SC_ZERO_TIME, policy.quantum()}) {

    SC_METHOD(sweep_helper);
    dont_initialize();
    sensitive << m_sweep_ev;

    SC_METHOD(step_helper);
    dont_initialize();
    sensitive << m_step_ev << m_update_ev;

    m_step_ev.notify(policy.quantum());

    this->sc_core::sc_prim_channel::async_attach_suspending();
  }
};

} // namespace sc_core

#endif // FSS_SYSTEMC_TLM_SC_SYNC_WINDOW_H
