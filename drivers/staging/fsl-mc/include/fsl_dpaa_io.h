/* Copyright 2014 Freescale Semiconductor Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Freescale Semiconductor nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY Freescale Semiconductor ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Freescale Semiconductor BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef __FSL_DPAA_IO_H
#define __FSL_DPAA_IO_H

#include "fsl_dpaa_fd.h"

struct dpaa_io;
struct dpaa_io_store;

/***************************/
/* DPIO Service management */
/***************************/

struct dpaa_io_desc {
	/* non-zero iff the DPIO has a channel */
	int receives_notifications;
	/* non-zero if the DPIO portal interrupt is handled. If so, the
	 * caller/OS handles the interrupt and calls dpaa_io_service_irq(). */
	int has_irq;
	/* non-zero if the caller/OS is prepared to called the
	 * dpaa_io_service_poll() routine as part of its run-to-completion (or
	 * scheduling) loop. If so, the DPIO service may dynamically switch some
	 * of its processing between polling-based and irq-based. It is illegal
	 * combination to have (!has_irq && !will_poll). */
	int will_poll;
	/* ignored unless 'receives_notifications'. Non-zero iff the channel has
	 * 8 priority WQs, otherwise the channel has 2. */
	int has_8prio;
	/* the cpu index that at least interrupt handlers will execute on. And
	 * if 'stash_affinity' is non-zero, the cache targeted by stash
	 * transactions is affine to this cpu. */
	int cpu;
	/* non-zero if stash transactions for this portal favour 'cpu' over
	 * other CPUs. (Eg. zero if there's no stashing, or stashing is to
	 * shared cache.) */
	int stash_affinity;
	/* Caller-provided flags, determined by bus-scanning and/or creation of
	 * DPIO objects via MC commands. */
	void *regs_cena;
	void *regs_cinh;
	int dpio_id;
};

/* Activates a "struct dpaa_io" corresponding to the given config of an actual
 * DPIO object. This handle can be used on it's own (like a one-portal "DPIO
 * service") or later be added to a service-type "struct dpaa_io" object. Note,
 * the information required on 'cfg' is copied so the caller is free to do as
 * they wish with the input parameter upon return. */
struct dpaa_io *dpaa_io_create(const struct dpaa_io_desc *desc);

/* Create an (initially empty) DPIO service. */
struct dpaa_io *dpaa_io_create_service(void);

/* Or use the driver's own global (and initially empty) DPIO service. This
 * increments the reference count, so don't forget to use dpaa_io_down() for
 * each time this function is called. */
struct dpaa_io *dpaa_io_default_service(void);

/* The "struct dpaa_io" type can represent an individual DPIO object (as
 * described by "struct dpaa_io_desc") or an instance of a "DPIO service", which
 * can be used to group/encapsulate multiple DPIO objects. In all cases, each
 * handle obtained should be released using this function. */
void dpaa_io_down(struct dpaa_io *);

/* Add the given DPIO object to the given DPIO service. 'service' must have been
 * created by dpaa_io_create_service() and 'obj' must have been created by
 * dpaa_io_create(). This increments the reference count on the object that
 * 'obj' refers to, so the user could call dpaa_io_down(obj) after this and the
 * object will persist within the service (and will be destroyed when the
 * service is destroyed). */
int dpaa_io_service_add(struct dpaa_io *service, struct dpaa_io *obj);

/* This function will return failure if the given dpaa_io struct represents a
 * service rather than an individual DPIO object, otherwise it returns zero and
 * the given 'cfg' structure is filled in. */
int dpaa_io_get_descriptor(struct dpaa_io *obj, struct dpaa_io_desc *desc);

/* Process any notifications and h/w-initiated events that are polling-driven.
 * Obligatory for DPIO objects that have dpaa_io_desc::will_poll non-zero. */
int dpaa_io_poll(struct dpaa_io *);

/* Process any notifications and h/w-initiated events that are irq-driven.
 * Obligatory for DPIO objects that have dpaa_io_desc::has_irq non-zero. */
int dpaa_io_preirq(struct dpaa_io *);
int dpaa_io_irq(struct dpaa_io *);

/* If a polling application is going to stop polling for a period of time and
 * supports interrupt processing, it can call this function to convert all
 * processing to IRQ. (Eg. when sleeping.) */
int dpaa_io_pause_poll(struct dpaa_io *);

/* And likewise, when polling will resume; */
int dpaa_io_resume_poll(struct dpaa_io *);

/* Get a mask of cpus that the DPIO service can receive notifications on. Note
 * that this is a run-time snapshot. If things like cpu-hotplug are supported in
 * the target system, then an attempt to register notifications for a cpu that
 * appears present in the given mask might fail if that cpu has gone offline in
 * the mean time. */
void dpaa_io_service_notifications(struct dpaa_io *, cpumask_t *mask);

/* Get a mask of cpus that the DPIO service has stash affinity to. */
void dpaa_io_service_stashing(struct dpaa_io *, cpumask_t *mask);

/* Return a boolean, whether or not the DPIO service has resources that have no
 * particular cpu affinity for stashing. (Useful to know if you wish to operate
 * on CPUs that the service has no affinity to, you would choose to use
 * resources that are neutral, rather than affine to a different CPU.) Unlike
 * other service-specific APIs, this one doesn't return an error if it is passed
 * a non-service object. So don't do it. */
int dpaa_io_service_has_nonaffine(struct dpaa_io *);

/*************************/
/* Notification handling */
/*************************/

/* When a FQDAN/CDAN registration is made (eg. by DPNI/DPCON/DPAI code), a
 * context of the following type is used. The caller can embed it within a
 * larger structure in order to add state that is tracked along with the
 * notification (this may be useful when callbacks are invoked that pass this
 * notification context as a parameter).
 */
struct dpaa_io_notification_ctx {
	/* the callback to be invoked when the notification arrives */
	void (*cb)(struct dpaa_io_notification_ctx *);
	/* Zero/FALSE for FQDAN, non-zero/TRUE for CDAN */
	int is_cdan;
	uint32_t id; /* FQID or channel ID, needed for rearm */
	/* This specifies which cpu the user wants notifications to show up on
	 * (ie. to execute 'cb'). If notification-handling on that cpu is not
	 * available at the time of notification registration, the registration
	 * will fail. */
	int desired_cpu;
	/* If the target platform supports cpu-hotplug or other features
	 * (related to power-management, one would expect) that can migrate IRQ
	 * handling of a given DPIO object, then this value will potentially be
	 * different to 'desired_cpu' at run-time. */
	int actual_cpu;
	/* And if migration does occur and this callback is non-NULL, it will
	 * be invoked prior to any futher notification callbacks executing on
	 * 'newcpu'. Note that 'oldcpu' is what 'actual_cpu' was prior to the
	 * migration, and 'newcpu' is what it is now. Both could conceivably be
	 * different to 'desired_cpu'. */
	void (*migration_cb)(struct dpaa_io_notification_ctx *,
			     int oldcpu, int newcpu);
	/* These are returned from dpaa_io_service_register().
	 * 'dpio_id' is the dpaa_io_desc::dpio_id value of the DPIO object that
	 * has been selected by the service for receiving the notifications. The
	 * caller can use this value in the MC command that attaches the FQ (or
	 * channel) of their DPNI (or DPCON, respectively) to this DPIO for
	 * notification-generation.
	 * 'qman64' is the 64-bit context value that needs to be sent in the
	 * same MC command in order to be programmed into the FQ or channel -
	 * this is the 64-bit value that shows up in the FQDAN/CDAN messages to
	 * the DPIO object, and the DPIO service specifies this value back to
	 * the caller so that the notifications that show up will be
	 * comprensible/demux-able to the DPIO service. */
	int dpio_id;
	uint64_t qman64;
	/* These fields are internal to the DPIO service once the context is
	 * registered. TBD: may require more internal state fields. */
	struct list_head node;
	void *dpio_private;
};

/* Prepare for servicing of FQDAN or CDAN notifications on the given DPIO
 * service. The MC command to attach the caller's DPNI/DPCON/DPAI device to a
 * DPIO object is performed after this function is called. In that way, (a) the
 * DPIO service is "ready" to handle a notification arrival (which might happen
 * before the "attach" command to MC has returned control of execution back to
 * the caller), and (b) the DPIO service can provide back to the caller the
 * 'dpio_id' and 'qman64' parameters that it should pass along in the MC command
 * in order for the DPNI/DPCON/DPAI resources to be configured to produce the
 * right notification fields to the DPIO service. */
int dpaa_io_service_register(struct dpaa_io *service,
			     struct dpaa_io_notification_ctx *ctx);

/* The opposite of 'register'. Whereas 'register' should be called *before*
 * making the MC call to attach the notification-producing device to the
 * notification-handling DPIO service, the 'unregister' function should be
 * called *after* making the MC call to detach the notification-producing
 * device. */
int dpaa_io_service_deregister(struct dpaa_io *service,
			       struct dpaa_io_notification_ctx *ctx);

/* Once a FQDAN/CDAN has been produced, the corresponding FQ/channel is
 * considered "disarmed". Ie. the user can issue pull dequeue operations on that
 * traffic source for as long as it likes. Eventually it may wish to "rearm"
 * that source to allow it to produce another FQDAN/CDAN, that's what this
 * function achieves. */
int dpaa_io_service_rearm(struct dpaa_io *service,
			  struct dpaa_io_notification_ctx *ctx);

/* Like 'dpaa_io_service_get_persistent()' (see below), except that the returned
 * handle is not selected based on a 'cpu' argument, but is the same DPIO object
 * that the given notification context is registered against. The returned
 * handle carries a reference count, so a corresponding dpaa_io_down() would be
 * required when the reference is no longer needed.
 */
int dpaa_io_from_registration(struct dpaa_io_notification_ctx *ctx,
			      struct dpaa_io **ret);

/**********************************/
/* General usage of DPIO services */
/**********************************/

/* The various DPIO interfaces can accept a "struct dpaa_io" handle that refers
 * to an individual DPIO object or to a whole service. In the latter case, an
 * internal choice is made for each operation. This function supports the former
 * case, by selecting an individual DPIO object *from* the service in order for
 * it to be used multiple times to provide "persistence". The returned handle
 * also carries a reference count, so a corresponding dpaa_io_down() would be
 * required when the reference is no longer needed. Note, a parameter of -1 for
 * 'cpu' will select a DPIO resource that has no particular stashing affinity to
 * any cpu (eg. one that stashes to platform cache). */
int dpaa_io_service_get_persistent(struct dpaa_io *service, int cpu,
				   struct dpaa_io **ret);

/*****************/
/* Pull dequeues */
/*****************/

/* To support DCA/order-preservation, it will be necessary to support an
 * alternative form, because they must ultimately dequeue to DQRR rather than a
 * user-supplied dpaa_io_store. Furthermore, those dequeue results will
 * "complete" using a caller-provided callback (from DQRR processing) rather
 * than the caller explicitly looking at their dpaa_io_store for results. Eg.
 * the alternative form will likely take a callback parameter rather than a
 * store parameter. Ignoring it for now to keep the picture clearer. */
int dpaa_io_service_pull_fq(struct dpaa_io *, uint32_t fqid,
			    struct dpaa_io_store *);
int dpaa_io_service_pull_channel(struct dpaa_io *, uint32_t channelid,
				 struct dpaa_io_store *);

/************/
/* Enqueues */
/************/

/* This definition bypasses some features that are not expected to be priority-1
 * features, and may not be needed at all via current assumptions (QBMan's
 * feature set is wider than the MC object model is intendeding to support,
 * initially at least). Plus, keeping them out (for now) keeps the API view
 * simpler. Missing features are;
 *  - enqueue confirmation (results DMA'd back to the user)
 *  - ORP
 *  - DCA/order-preservation (see note in "pull dequeues")
 *  - enqueue consumption interrupts
 */
int dpaa_io_service_enqueue_fq(struct dpaa_io *,
			       uint32_t fqid,
			       const struct dpaa_fd *fd);
int dpaa_io_service_enqueue_qd(struct dpaa_io *,
			       uint32_t qdid, uint8_t prio, uint16_t qdbin,
			       const struct dpaa_fd *fd);

/*******************/
/* Buffer handling */
/*******************/

/* Release buffers to a buffer pool */
int dpaa_io_service_release(struct dpaa_io *,
			    uint32_t bpid,
			    const uint64_t *buffers,
			    unsigned int num_buffers);

/* Acquire buffers from a buffer pool, returns a negative error code if the
 * command failed, otherwise it returns the number of buffers acquired, which
 * may be less than the number requested. Eg. if the buffer pool is empty, this
 * will return zero. */
int dpaa_io_service_acquire(struct dpaa_io *,
			    uint32_t bpid,
			    uint64_t *buffers,
			    unsigned int num_buffers);

/***************/
/* DPIO stores */
/***************/

/* These are reusable memory blocks for retrieving dequeue results into, and to
 * assist with parsing those results once they show up. They also hide the
 * details of how to use "tokens" to make detection of DMA results possible (ie.
 * comparing memory before the DMA and after it) while minimising the needless
 * clearing/rewriting of those memory locations between uses.
 */

/* Constructor/destructor - max_frames must be <= 16. The user provides the
 * device struct to allow mapping/unmapping of the DMAable region. Area for
 * storage will be allocated during create. The size of this storage is
 * "max_frames*sizeof(struct ldpaa_dq)". The 'dpaa_io_store' returned is a
 * wrapper structure allocated within the DPIO code, which owns and manages
 * allocated store.
 */
struct dpaa_io_store *dpaa_io_store_create(unsigned int max_frames,
					   struct device *dev);
void dpaa_io_store_destroy(struct dpaa_io_store *);

/* Once dpaa_io_store has been passed to a function that performs dequeues to
 * it, like dpaa_ni_rx(), this function can be used to determine when the next
 * frame result is available. Once this function returns non-NULL, a subsequent
 * call to it will try to find the *next* dequeue result.
 *
 * Note that if a pull-dequeue has a null result because the target FQ/channel
 * was empty, then this function will return NULL rather than expect the caller
 * to always check for this on his own side. As such, "is_last" can be used to
 * differentiate between "end-of-empty-dequeue" and "still-waiting".
 */
struct ldpaa_dq *dpaa_io_store_next(struct dpaa_io_store *, int *is_last);

#ifdef CONFIG_FSL_QBMAN_DEBUG
/* Knowing the FQ count at run-time can be useful in debugging situations.
 * The instantaneous frame- and byte-count are hereby returned.
 */
int dpaa_io_query_fq_count(struct dpaa_io *s, uint32_t fqid,
			   uint32_t *fcnt, uint32_t *bcnt);
#endif
#endif /* __FSL_DPAA_IO_H */
