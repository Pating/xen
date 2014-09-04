/*
 * Copyright (C) 2014 FUJITSU LIMITED
 * Author: Wen Congyang <wency@cn.fujitsu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"
#include "libxl_colo.h"
#include "../libxc/xg_private.h"
#include "../libxc/xc_bitops.h"

enum {
    LIBXL_COLO_SETUPED,
    LIBXL_COLO_SUSPENDED,
    LIBXL_COLO_RESUMED,
};

typedef struct libxl__colo_restore_checkpoint_state libxl__colo_restore_checkpoint_state;
struct libxl__colo_restore_checkpoint_state {
    xc_hypercall_buffer_t _dirty_bitmap;
    xc_hypercall_buffer_t *dirty_bitmap;
    unsigned long p2m_size;
    libxl__domain_suspend_state2 dss2;
    /* for sending data to master */
    libxl__datacopier_state dc;
    /* for reading data from master */
    libxl__datareader_state drs;
    uint8_t section;
    libxl__logdirty_switch lds;
    libxl__colo_restore_state *crs;
    int status;
    /* used for teardown */
    int teardown_devices;
    int saved_rc;

    void (*callback)(libxl__egc *,
                     libxl__colo_restore_checkpoint_state *,
                     int);

    /*
     * 0: secondary vm's dirty bitmap for domain @domid
     * 1: secondary vm is ready(domain @domid)
     * 2: secondary vm is resumed(domain @domid)
     */
    const char *copywhat[3];
};


static void libxl__colo_restore_domain_resume_callback(void *data);
static void libxl__colo_restore_domain_checkpoint_callback(void *data);
static void libxl__colo_restore_domain_suspend_callback(void *data);

extern const libxl__checkpoint_device_instance_ops colo_restore_device_nic;

static const libxl__checkpoint_device_instance_ops *colo_restore_ops[] = {
    &colo_restore_device_nic,
    NULL,
};

/* ===================== colo: common functions ===================== */
static void colo_enable_logdirty(libxl__colo_restore_state *crs, libxl__egc *egc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    const uint32_t domid = crs->domid;
    libxl__logdirty_switch *const lds = &crcs->lds;

    STATE_AO_GC(crs->ao);

    /* we need to know which pages are dirty to restore the guest */
    if (xc_shadow_control(CTX->xch, domid,
                          XEN_DOMCTL_SHADOW_OP_ENABLE_LOGDIRTY,
                          NULL, 0, NULL, 0, NULL) < 0) {
        LOG(ERROR, "cannot enable secondary vm's logdirty");
        lds->callback(egc, lds, ERROR_FAIL);
        return;
    }

    if (crs->hvm) {
        libxl__domain_common_switch_qemu_logdirty(domid, 1, lds, egc);
        return;
    }

    lds->callback(egc, lds, 0);
}

static void colo_disable_logdirty(libxl__colo_restore_state *crs,
                                  libxl__egc *egc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    const uint32_t domid = crs->domid;
    libxl__logdirty_switch *const lds = &crcs->lds;

    STATE_AO_GC(crs->ao);

    /* we need to know which pages are dirty to restore the guest */
    if (xc_shadow_control(CTX->xch, domid, XEN_DOMCTL_SHADOW_OP_OFF,
                          NULL, 0, NULL, 0, NULL) < 0)
        LOG(WARN, "cannot disable secondary vm's logdirty");

    if (crs->hvm) {
        libxl__domain_common_switch_qemu_logdirty(domid, 0, lds, egc);
        return;
    }

    lds->callback(egc, lds, 0);
}

static void colo_resume_vm(libxl__egc *egc,
                           libxl__colo_restore_checkpoint_state *crcs,
                           int restore_device_model)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int rc;

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;

    STATE_AO_GC(crs->ao);

    if (!crs->saved_cb) {
        /* TODO: sync mmu for hvm? */
        if (restore_device_model) {
            rc = libxl__domain_restore(gc, crs->domid);
            if (rc) {
                LOG(ERROR, "cannot restore device model for secondary vm");
                crcs->callback(egc, crcs, rc);
                return;
            }
        }
        rc = libxl__domain_resume(gc, crs->domid, 0);
        if (rc)
            LOG(ERROR, "cannot resume secondary vm");

        crcs->callback(egc, crcs, rc);
        return;
    }

    libxl__xc_domain_restore_done(egc, dcs, 0, 0, 0);

    return;
}

static int init_device_subkind(libxl__checkpoint_devices_state *cds)
{
    /* init device subkind-specific state in the libxl ctx */
    int rc;
    STATE_AO_GC(cds->ao);

    rc = init_subkind_colo_nic(cds);
    if (rc) goto out;

    rc = 0;
out:
    return rc;
}

static void cleanup_device_subkind(libxl__checkpoint_devices_state *cds)
{
    /* cleanup device subkind-specific state in the libxl ctx */
    STATE_AO_GC(cds->ao);

    cleanup_subkind_colo_nic(cds);
}


/* ================ colo: setup restore environment ================ */
static void libxl__colo_domain_create_cb(libxl__egc *egc,
                                         libxl__domain_create_state *dcs,
                                         int rc, uint32_t domid);

static int init_dss2(libxl__domain_suspend_state2 *dss2)
{
    int rc = ERROR_FAIL;
    libxl_domain_type type;

    STATE_AO_GC(dss2->ao);

    type = libxl__domain_type(gc, dss2->domid);
    if (type == LIBXL_DOMAIN_TYPE_INVALID)
        goto out;

    libxl__xswait_init(&dss2->pvcontrol);
    libxl__ev_evtchn_init(&dss2->guest_evtchn);
    libxl__ev_xswatch_init(&dss2->guest_watch);
    libxl__ev_time_init(&dss2->guest_timeout);

    if (type == LIBXL_DOMAIN_TYPE_HVM)
        dss2->hvm = 1;
    else
        dss2->hvm = 0;

    dss2->guest_evtchn.port = -1;
    dss2->guest_evtchn_lockfd = -1;
    dss2->guest_responded = 0;
    dss2->dm_savefile = libxl__device_model_savefile(gc, dss2->domid);
    dss2->save_dm = 0;

    /* Secondary vm is not created, so we cannot get evtchn port */

    rc = 0;

out:
    return rc;
}

void libxl__colo_restore_setup(libxl__egc *egc,
                               libxl__colo_restore_state *crs)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs;
    DECLARE_HYPERCALL_BUFFER(unsigned long, dirty_bitmap);
    int rc = ERROR_FAIL;
    int bsize;

    /* Convenience aliases */
    libxl__srm_restore_autogen_callbacks *const callbacks =
        &dcs->shs.callbacks.restore.a;
    const int domid = crs->domid;

    STATE_AO_GC(crs->ao);

    GCNEW(crcs);
    crs->crcs = crcs;
    crcs->crs = crs;

    crcs->p2m_size = xc_domain_maximum_gpfn(CTX->xch, domid) + 1;

    crcs->copywhat[0] = GCSPRINTF("secondary vm's dirty bitmap for domain %"PRIu32,
                                  domid);
    crcs->copywhat[1] = GCSPRINTF("secondary vm is ready(domain %"PRIu32")",
                                  domid);
    crcs->copywhat[2] = GCSPRINTF("secondary vm is resumed(domain %"PRIu32")",
                                  domid);

    bsize = bitmap_size(crcs->p2m_size);
    dirty_bitmap = xc_hypercall_buffer_alloc_pages(CTX->xch, dirty_bitmap,
                                                   NRPAGES(bsize));
    if (!dirty_bitmap) {
        rc = ERROR_NOMEM;
        goto err;
    }
    memset(dirty_bitmap, 0, bsize);
    crcs->_dirty_bitmap = *HYPERCALL_BUFFER(dirty_bitmap);
    crcs->dirty_bitmap = &crcs->_dirty_bitmap;

    /* setup dss2 */
    crcs->dss2.ao = ao;
    crcs->dss2.domid = domid;
    if (init_dss2(&crcs->dss2))
        goto err_init_dss2;

    callbacks->suspend = libxl__colo_restore_domain_suspend_callback;
    callbacks->postcopy = libxl__colo_restore_domain_resume_callback;
    callbacks->checkpoint = libxl__colo_restore_domain_checkpoint_callback;

    /*
     * Secondary vm is running in colo mode, so we need to call
     * libxl__xc_domain_restore_done() to create secondary vm.
     * But we will exit in domain_create_cb(). So replace the
     * callback here.
     */
    crs->saved_cb = dcs->callback;
    dcs->callback = libxl__colo_domain_create_cb;
    crcs->status = LIBXL_COLO_SETUPED;

    logdirty_init(&crcs->lds);
    crcs->lds.ao = ao;

    rc = 0;

out:
    crs->callback(egc, crs, rc);
    return;

err_init_dss2:
    xc_hypercall_buffer_free_pages(CTX->xch, dirty_bitmap, NRPAGES(bsize));
    crcs->dirty_bitmap = NULL;
err:
    goto out;
}

static void libxl__colo_domain_create_cb(libxl__egc *egc,
                                         libxl__domain_create_state *dcs,
                                         int rc, uint32_t domid)
{
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    crcs->callback(egc, crcs, rc);
}


/* ================ colo: teardown restore environment ================ */
static void colo_restore_teardown_done(libxl__egc *egc,
                                       libxl__checkpoint_devices_state *cds,
                                       int rc);
static void do_failover_done(libxl__egc *egc,
                             libxl__colo_restore_checkpoint_state* crcs,
                             int rc);
static void colo_disable_logdirty_done(libxl__egc *egc,
                                       libxl__logdirty_switch *lds,
                                       int rc);

static void do_failover(libxl__egc *egc, libxl__colo_restore_state *crs)
{
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    const int status = crcs->status;
    libxl__logdirty_switch *const lds = &crcs->lds;

    STATE_AO_GC(crs->ao);

    switch(status) {
    case LIBXL_COLO_SETUPED:
        /* We don't enable logdirty now */
        colo_resume_vm(egc, crcs, 0);
        return;
    case LIBXL_COLO_SUSPENDED:
    case LIBXL_COLO_RESUMED:
        /* disable logdirty first */
        lds->callback = colo_disable_logdirty_done;
        colo_disable_logdirty(crs, egc);
        return;
    default:
        LOG(ERROR, "invalid status: %d", status);
        crcs->callback(egc, crcs, ERROR_FAIL);
    }
}

void libxl__colo_restore_teardown(libxl__egc *egc,
                                  libxl__colo_restore_state *crs,
                                  int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap, crcs->dirty_bitmap);
    int bsize = bitmap_size(crcs->p2m_size);
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);

    EGC_GC;

    if (!dirty_bitmap)
        goto teardown_devices;

    xc_hypercall_buffer_free_pages(CTX->xch, dirty_bitmap, NRPAGES(bsize));

teardown_devices:
    crcs->saved_rc = rc;
    if (!crcs->teardown_devices) {
        colo_restore_teardown_done(egc, &crs->cds, 0);
        return;
    }

    crs->cds.callback = colo_restore_teardown_done;
    libxl__checkpoint_devices_teardown(egc, &crs->cds);
}

static void colo_restore_teardown_done(libxl__egc *egc,
                                       libxl__checkpoint_devices_state *cds,
                                       int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);

    EGC_GC;

    if (rc)
        LOG(ERROR, "COLO: failed to teardown device after setup failed"
            " for guest with domid %u, rc %d", cds->domid, rc);

    cleanup_device_subkind(cds);

    rc = crcs->saved_rc;
    if (!rc) {
        crcs->callback = do_failover_done;
        do_failover(egc, crs);
        return;
    }

    if (crs->saved_cb) {
        dcs->callback = crs->saved_cb;
        crs->saved_cb = NULL;
    }
    crs->callback(egc, crs, rc);
}

static void do_failover_done(libxl__egc *egc,
                             libxl__colo_restore_checkpoint_state* crcs,
                             int rc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;

    STATE_AO_GC(crs->ao);

    if (rc)
        LOG(ERROR, "cannot do failover");

    if (crs->saved_cb) {
        dcs->callback = crs->saved_cb;
        crs->saved_cb = NULL;
    }

    crs->callback(egc, crs, rc);
}

static void colo_disable_logdirty_done(libxl__egc *egc,
                                       libxl__logdirty_switch *lds,
                                       int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);

    STATE_AO_GC(lds->ao);

    if (rc)
        LOG(WARN, "cannot disable logdirty");

    if (crcs->status == LIBXL_COLO_SUSPENDED) {
        /*
         * failover when reading state from master, so no need to
         * call libxl__domain_restore().
         */
        colo_resume_vm(egc, crcs, 0);
        return;
    }

    /* If we cannot disable logdirty, we still can do failover */
    crcs->callback(egc, crcs, 0);
}

/*
 * checkpoint callbacks are called in the following order:
 * 1. resume
 * 2. checkpoint
 * 3. suspend
 */
static void colo_common_send_data_done(libxl__egc *egc,
                                       libxl__datacopier_state *dc,
                                       int onwrite, int errnoval);
/* ===================== colo: resume secondary vm ===================== */
/*
 * Do the following things when resuming secondary vm:
 *  1. write LIBXL_COLO_SVM_READY
 *  2. resume secondary vm
 *  3. write LIBXL_COLO_SVM_RESUMED
 */
static void colo_send_svm_ready_done(libxl__egc *egc,
                                     libxl__colo_restore_checkpoint_state *crcs,
                                     int rc);
static void colo_resume_vm_done(libxl__egc *egc,
                                libxl__colo_restore_checkpoint_state *crcs,
                                int rc);
static void colo_write_svm_resumed(libxl__egc *egc,
                                   libxl__colo_restore_checkpoint_state *crcs);
static void colo_enable_logdirty_done(libxl__egc *egc,
                                      libxl__logdirty_switch *lds,
                                      int retval);
static void colo_reenable_logdirty(libxl__egc *egc,
                                   libxl__logdirty_switch *lds,
                                   int rc);
static void colo_reenable_logdirty_done(libxl__egc *egc,
                                        libxl__logdirty_switch *lds,
                                        int rc);
static void colo_setup_checkpoint_devices(libxl__egc *egc,
                                          libxl__colo_restore_state *crs);
static void colo_restore_setup_cds_done(libxl__egc *egc,
                                        libxl__checkpoint_devices_state *cds,
                                        int rc);

static void libxl__colo_restore_domain_resume_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;
    uint8_t section = LIBXL_COLO_SVM_READY;
    int rc;

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = &dcs->crs;
    const int send_fd = crs->send_fd;
    libxl__datacopier_state *const dc = &crcs->dc;

    STATE_AO_GC(crs->ao);

    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = -1;
    dc->writefd = send_fd;
    dc->maxsz = INT_MAX;
    dc->copywhat = crcs->copywhat[1];
    dc->writewhat = "colo stream";
    dc->callback = colo_common_send_data_done;
    crcs->callback = colo_send_svm_ready_done;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    /* tell master that secondary vm is ready */
    libxl__datacopier_prefixdata(shs->egc, dc, &section, sizeof(section));

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(shs->egc, shs, 0);
}

static void colo_send_svm_ready_done(libxl__egc *egc,
                                     libxl__colo_restore_checkpoint_state *crcs,
                                     int rc)
{
    crcs->callback = colo_resume_vm_done;
    colo_resume_vm(egc, crcs, 1);

    return;
}

static void colo_resume_vm_done(libxl__egc *egc,
                                libxl__colo_restore_checkpoint_state *crcs,
                                int rc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;
    libxl__logdirty_switch *const lds = &crcs->lds;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        LOG(ERROR, "cannot resume secondary vm");
        goto out;
    }

    crcs->status = LIBXL_COLO_RESUMED;

    /* avoid calling libxl__xc_domain_restore_done() more than once */
    if (crs->saved_cb) {
        dcs->callback = crs->saved_cb;
        crs->saved_cb = NULL;

        lds->callback = colo_enable_logdirty_done;
        colo_enable_logdirty(crs, egc);
        return;
    }

    colo_write_svm_resumed(egc, crcs);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_write_svm_resumed(libxl__egc *egc,
                                   libxl__colo_restore_checkpoint_state *crcs)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    uint8_t section = LIBXL_COLO_SVM_RESUMED;
    int rc;

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;
    const int send_fd = crs->send_fd;
    libxl__datacopier_state *const dc = &crcs->dc;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = -1;
    dc->writefd = send_fd;
    dc->maxsz = INT_MAX;
    dc->copywhat = crcs->copywhat[2];
    dc->writewhat = "colo stream";
    dc->callback = colo_common_send_data_done;
    crcs->callback = NULL;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    /* tell master that secondary vm is resumed */
    libxl__datacopier_prefixdata(egc, dc, &section, sizeof(section));

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_enable_logdirty_done(libxl__egc *egc,
                                      libxl__logdirty_switch *lds,
                                      int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        /*
         * log-dirty already enabled? There's no test op,
         * so attempt to disable then reenable it
         */
        lds->callback = colo_reenable_logdirty;
        colo_disable_logdirty(crs, egc);
        return;
    }

    colo_setup_checkpoint_devices(egc, crs);
}

static void colo_reenable_logdirty(libxl__egc *egc,
                                   libxl__logdirty_switch *lds,
                                   int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        LOG(ERROR, "cannot enable logdirty");
        goto out;
    }

    lds->callback = colo_reenable_logdirty_done;
    colo_enable_logdirty(crs, egc);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_reenable_logdirty_done(libxl__egc *egc,
                                        libxl__logdirty_switch *lds,
                                        int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crcs->crs->ao);

    if (rc) {
        LOG(ERROR, "cannot enable logdirty");
        goto out;
    }

    colo_setup_checkpoint_devices(egc, crcs->crs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

/*
 * We cannot setup checkpoint devices in libxl__colo_restore_setup(),
 * because the guest is not ready.
 */
static void colo_setup_checkpoint_devices(libxl__egc *egc,
                                          libxl__colo_restore_state *crs)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    libxl__checkpoint_devices_state *cds = &crs->cds;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    crcs->teardown_devices = 1;

    cds->device_kind_flags = (1 << LIBXL__DEVICE_KIND_VIF);
    cds->callback = colo_restore_setup_cds_done;
    cds->ao = ao;
    cds->domid = crs->domid;
    cds->ops = colo_restore_ops;

    if (init_device_subkind(cds))
        goto out;

    libxl__checkpoint_devices_setup(egc, cds);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_restore_setup_cds_done(libxl__egc *egc,
                                        libxl__checkpoint_devices_state *cds,
                                        int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    libxl__save_helper_state *const shs = &dcs->shs;
    const uint32_t domid = crs->domid;

    STATE_AO_GC(cds->ao);

    if (rc) {
        LOG(ERROR, "COLO: failed to setup device for guest with domid %u",
            cds->domid);
        goto out;
    }

    /* We have enabled secondary vm's logdirty, so we can unpause it now */
    rc = libxl__domain_unpause(gc, domid);
    if (rc) {
        LOG(ERROR, "cannot unpause secondary vm");
        goto out;
    }

    colo_write_svm_resumed(egc, crcs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}


/* ===================== colo: wait new checkpoint ===================== */
static void colo_stream_read_done(libxl__egc *egc,
                                  libxl__datareader_state *drs,
                                  ssize_t real_size, int errnoval);

static void libxl__colo_restore_domain_checkpoint_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    /* Convenience aliases */
    const int recv_fd = dcs->crs.recv_fd;
    libxl__datareader_state *const drs = &crcs->drs;

    STATE_AO_GC(dcs->crs.ao);

    memset(drs, 0, sizeof(*drs));
    drs->ao = ao;
    drs->readfd = recv_fd;
    drs->readsize = sizeof(crcs->section);
    drs->readwhat = "colo stream";
    drs->callback = colo_stream_read_done;
    drs->buf = &crcs->section;

    if (libxl__datareader_start(drs)) {
        LOG(ERROR, "libxl__datareader_start() fails");
        goto out;
    }

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(shs->egc, shs, 0);
}

static void colo_stream_read_done(libxl__egc *egc,
                                  libxl__datareader_state *drs,
                                  ssize_t real_size, int errnoval)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(drs, *crcs, drs);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int ok = 0;

    /* Convenience aliases */
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(drs->ao);

    if (real_size < drs->readsize) {
        LOG(ERROR, "reading data fails: %lld", (long long)real_size);
        goto out;
    }

    if (crcs->section != LIBXL_COLO_NEW_CHECKPOINT) {
        LOG(ERROR, "invalid section: %d", crcs->section);
        goto out;
    }

    ok = 1;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, ok);
}


/* ===================== colo: suspend secondary vm ===================== */
/*
 * Do the following things when resuming secondary vm:
 *  1. suspend secondary vm
 *  2. get secondary vm's dirty page information
 *  3. send LIBXL_COLO_SVM_SUSPENDED
 *  4. send secondary vm's dirty page information(count + pfn list)
 */
static void colo_suspend_vm_done(libxl__egc *egc,
                                 libxl__domain_suspend_state2 *dss2,
                                 int ok);
static void colo_append_pfn_type(libxl__egc *egc,
                                 libxl__datacopier_state *dc,
                                 unsigned long *dirty_bitmap,
                                 unsigned long p2m_size);

static void libxl__colo_restore_domain_suspend_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    STATE_AO_GC(dcs->ao);

    /* Convenience aliases */
    libxl__domain_suspend_state2 *const dss2 = &crcs->dss2;

    /* suspend secondary vm */
    dss2->callback_common_done = colo_suspend_vm_done;

    libxl__domain_suspend2(shs->egc, dss2);
}

static void colo_suspend_vm_done(libxl__egc *egc,
                                 libxl__domain_suspend_state2 *dss2,
                                 int ok)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(dss2, *crcs, dss2);
    libxl__colo_restore_state *crs = crcs->crs;
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap, crcs->dirty_bitmap);
    uint8_t section = LIBXL_COLO_SVM_SUSPENDED;
    int i, rc;
    uint64_t count;

    /* Convenience aliases */
    const int send_fd = crs->send_fd;
    const unsigned long p2m_size = crcs->p2m_size;
    const uint32_t domid = crs->domid;
    libxl__datacopier_state *const dc = &crcs->dc;

    STATE_AO_GC(crs->ao);

    if (!ok) {
        LOG(ERROR, "cannot suspend secondary vm");
        goto out;
    }

    crcs->status = LIBXL_COLO_SUSPENDED;

    /*
     * Secondary vm is running, so there are some dirty pages
     * that are non-dirty in master. Get dirty bitmap and
     * send it to master.
     */
    if (xc_shadow_control(CTX->xch, domid, XEN_DOMCTL_SHADOW_OP_CLEAN,
                          HYPERCALL_BUFFER(dirty_bitmap), p2m_size,
                          NULL, 0, NULL) != p2m_size) {
        LOG(ERROR, "getting secondary vm's dirty bitmap fails");
        goto out;
    }

    count = 0;
    for (i = 0; i < p2m_size; i++) {
        if (test_bit(i, dirty_bitmap))
            count++;
    }

    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = -1;
    dc->writefd = send_fd;
    dc->maxsz = INT_MAX;
    dc->copywhat = crcs->copywhat[0];
    dc->writewhat = "colo stream";
    dc->callback = colo_common_send_data_done;
    crcs->callback = NULL;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    /* tell master that secondary vm is suspended */
    libxl__datacopier_prefixdata(egc, dc, &section, sizeof(section));

    /* send dirty pages to master */
    libxl__datacopier_prefixdata(egc, dc, &count, sizeof(count));
    colo_append_pfn_type(egc, dc, dirty_bitmap, p2m_size);
    return;

out:
    ok = 0;
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}

static void colo_append_pfn_type(libxl__egc *egc,
                                 libxl__datacopier_state *dc,
                                 unsigned long *dirty_bitmap,
                                 unsigned long p2m_size)
{
    int i, count;
    /* Hack, buf->buf is private member... */
    libxl__datacopier_buf *buf = NULL;
    int max_batch = sizeof(buf->buf) / sizeof(uint64_t);
    int buf_size = max_batch * sizeof(uint64_t);
    uint64_t *pfn;

    STATE_AO_GC(dc->ao);

    pfn = libxl__zalloc(NOGC, buf_size);

    count = 0;
    for (i = 0; i < p2m_size; i++) {
        if (!test_bit(i, dirty_bitmap))
            continue;

        pfn[count++] = i;
        if (count == max_batch) {
            libxl__datacopier_prefixdata(egc, dc, pfn, buf_size);
            count = 0;
        }
    }

    if (count)
        libxl__datacopier_prefixdata(egc, dc, pfn, count * sizeof(uint64_t));

    free(pfn);
}


/* ===================== colo: common callback ===================== */
static void colo_common_send_data_done(libxl__egc *egc,
                                       libxl__datacopier_state *dc,
                                       int onwrite, int errnoval)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(dc, *crcs, dc);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int ok;
    STATE_AO_GC(dc->ao);

    if (onwrite == -1) {
        LOG(ERROR, "sending data fails");
        ok = 0;
        goto out;
    }

    if (errnoval) {
        /* failure happens when reading/writing, do failover? */
        ok = 2;
        goto out;
    }

    if (!crcs->callback) {
        /* Everythins is OK */
        ok = 1;
        goto out;
    }

    crcs->callback(egc, crcs, 0);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}
