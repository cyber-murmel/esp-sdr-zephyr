/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APP_CPU_H_
#define APP_CPU_H_

/*
 * Pin the cooperative and high priority system threads (network queues,
 * net_mgmt, USB, system work queue, Wi-Fi events) to @p cpu, so they never
 * delay time critical radio threads on the other one. Lower priority threads
 * (shell, logging) stay free. Call before USB starts (its controller
 * interrupt lands on the CPU that allocates it) and again after. No-op
 * without CONFIG_SCHED_CPU_MASK or on one CPU.
 */
void app_cpu_pin_system_threads(int cpu);

/*
 * Pin the calling thread to @p cpu, for good. It waits for the system work queue to do this. Without
 * it, a thread can come back from any wait on the other CPU, and what it then allocates an interrupt
 * for lands there. No-op without CONFIG_SCHED_CPU_MASK or on one CPU.
 */
void app_cpu_pin_self(int cpu);

#endif /* APP_CPU_H_ */
