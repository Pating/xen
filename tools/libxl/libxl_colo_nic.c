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

typedef struct libxl__colo_device_nic {
    int devid;
    const char *vif;
} libxl__colo_device_nic;

enum {
    primary,
    secondary,
};


/* ========== init() and cleanup() ========== */
int init_subkind_colo_nic(libxl__checkpoint_devices_state *cds)
{
    return 0;
}

void cleanup_subkind_colo_nic(libxl__checkpoint_devices_state *cds)
{
}

/* ========== helper functions ========== */
static void colo_save_setup_script_cb(libxl__egc *egc,
                                     libxl__async_exec_state *aes,
                                     int status);
static void colo_save_teardown_script_cb(libxl__egc *egc,
                                         libxl__async_exec_state *aes,
                                         int status);

/*
 * If the device has a vifname, then use that instead of
 * the vifX.Y format.
 * it must ONLY be used for remus because if driver domains
 * were in use it would constitute a security vulnerability.
 */
static const char *get_vifname(libxl__checkpoint_device *dev,
                               const libxl_device_nic *nic)
{
    const char *vifname = NULL;
    const char *path;
    int rc;

    STATE_AO_GC(dev->cds->ao);

    /* Convenience aliases */
    const uint32_t domid = dev->cds->domid;

    path = GCSPRINTF("%s/backend/vif/%d/%d/vifname",
                     libxl__xs_get_dompath(gc, 0), domid, nic->devid);
    rc = libxl__xs_read_checked(gc, XBT_NULL, path, &vifname);
    if (!rc && !vifname) {
        vifname = libxl__device_nic_devname(gc, domid,
                                            nic->devid,
                                            nic->nictype);
    }

    return vifname;
}

/*
 * the script needs the following env & args
 * $vifname
 * $XENBUS_PATH (/libxl/<domid>/colo_agent/<devid>/)
 * $forwarddev
 * $mode(primary/secondary)
 * $vmid
 * setup/teardown as command line arg.
 */
static void setup_async_exec(libxl__checkpoint_device *dev, char *op, int side,
                             char *colo_agent_script)
{
    int arraysize, nr = 0;
    char **env = NULL, **args = NULL;
    libxl__colo_device_nic *colo_nic = dev->concrete_data;
    libxl__checkpoint_devices_state *cds = dev->cds;
    libxl__async_exec_state *aes = &dev->aodev.aes;
    const libxl_device_nic *nic = dev->backend_dev;

    STATE_AO_GC(cds->ao);

    /* Convenience aliases */
    const uint32_t domid = cds->domid;
    const int devid = colo_nic->devid;
    const char *const vif = colo_nic->vif;

    arraysize = 11;
    GCNEW_ARRAY(env, arraysize);
    env[nr++] = "vifname";
    env[nr++] = libxl__strdup(gc, vif);
    env[nr++] = "XENBUS_PATH";
    env[nr++] = GCSPRINTF("%s/colo_agent/%d",
                          libxl__xs_libxl_path(gc, domid), devid);
    env[nr++] = "forwarddev";
    env[nr++] = libxl__strdup(gc, nic->forwarddev);
    env[nr++] = "mode";
    if (side == primary)
        env[nr++] = "primary";
    else
        env[nr++] = "secondary";
    env[nr++] = "vmid";
    env[nr++] = GCSPRINTF("%u", domid);
    env[nr++] = NULL;
    assert(nr == arraysize);

    arraysize = 3; nr = 0;
    GCNEW_ARRAY(args, arraysize);
    args[nr++] = colo_agent_script;
    args[nr++] = op;
    args[nr++] = NULL;
    assert(nr == arraysize);

    aes->ao = dev->cds->ao;
    aes->what = GCSPRINTF("%s %s", args[0], args[1]);
    aes->env = env;
    aes->args = args;
    aes->timeout_ms = LIBXL_HOTPLUG_TIMEOUT * 1000;
    aes->stdfds[0] = -1;
    aes->stdfds[1] = -1;
    aes->stdfds[2] = -1;

    if (!strcmp(op, "teardown"))
        aes->callback = colo_save_teardown_script_cb;
    else
        aes->callback = colo_save_setup_script_cb;
}

/* ========== setup() and teardown() ========== */
static void colo_nic_setup(libxl__egc *egc, libxl__checkpoint_device *dev,
                           int side, char *colo_agent_script)
{
    int rc;
    libxl__colo_device_nic *colo_nic;
    const libxl_device_nic *nic = dev->backend_dev;

    STATE_AO_GC(dev->cds->ao);

    /*
     * thers's no subkind of nic devices, so nic ops is always matched
     * with nic devices, we begin to setup the nic device
     */
    dev->matched = 1;

    if (!nic->forwarddev) {
        rc = ERROR_FAIL;
        goto out;
    }

    GCNEW(colo_nic);
    dev->concrete_data = colo_nic;
    colo_nic->devid = nic->devid;
    colo_nic->vif = get_vifname(dev, nic);
    if (!colo_nic->vif) {
        rc = ERROR_FAIL;
        goto out;
    }

    setup_async_exec(dev, "setup", side, colo_agent_script);
    rc = libxl__async_exec_start(gc, &dev->aodev.aes);
    if (rc)
        goto out;

    return;

out:
    dev->aodev.rc = rc;
    dev->aodev.callback(egc, &dev->aodev);
}

static void colo_save_setup_script_cb(libxl__egc *egc,
                                      libxl__async_exec_state *aes,
                                      int status)
{
    libxl__ao_device *aodev = CONTAINER_OF(aes, *aodev, aes);
    libxl__checkpoint_device *dev = CONTAINER_OF(aodev, *dev, aodev);
    libxl__colo_device_nic *colo_nic = dev->concrete_data;
    libxl__checkpoint_devices_state *cds = dev->cds;
    const char *out_path_base, *hotplug_error = NULL;
    int rc;

    STATE_AO_GC(cds->ao);

    /* Convenience aliases */
    const uint32_t domid = cds->domid;
    const int devid = colo_nic->devid;
    const char *const vif = colo_nic->vif;

    out_path_base = GCSPRINTF("%s/colo_agent/%d",
                              libxl__xs_libxl_path(gc, domid), devid);

    rc = libxl__xs_read_checked(gc, XBT_NULL,
                                GCSPRINTF("%s/hotplug-error", out_path_base),
                                &hotplug_error);
    if (rc)
        goto out;

    if (hotplug_error) {
        LOG(ERROR, "colo_agent script %s setup failed for vif %s: %s",
            aes->args[0], vif, hotplug_error);
        rc = ERROR_FAIL;
        goto out;
    }

    if (status) {
        rc = ERROR_FAIL;
        goto out;
    }

    rc = 0;

out:
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static void colo_nic_teardown(libxl__egc *egc, libxl__checkpoint_device *dev,
                              int side, char *colo_agent_script)
{
    int rc;
    STATE_AO_GC(dev->cds->ao);

    setup_async_exec(dev, "teardown", side, colo_agent_script);

    rc = libxl__async_exec_start(gc, &dev->aodev.aes);
    if (rc)
        goto out;

    return;

out:
    dev->aodev.rc = rc;
    dev->aodev.callback(egc, &dev->aodev);
}

static void colo_save_teardown_script_cb(libxl__egc *egc,
                                         libxl__async_exec_state *aes,
                                         int status)
{
    int rc;
    libxl__ao_device *aodev = CONTAINER_OF(aes, *aodev, aes);

    if (status)
        rc = ERROR_FAIL;
    else
        rc = 0;

    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

/* ======== primary ======== */
static void colo_nic_save_setup(libxl__egc *egc, libxl__checkpoint_device *dev)
{
    libxl__colo_save_state *css = CONTAINER_OF(dev->cds, *css, cds);

    colo_nic_setup(egc, dev, primary, css->colo_agent_script);
}

static void colo_nic_save_teardown(libxl__egc *egc,
                                   libxl__checkpoint_device *dev)
{
    libxl__colo_save_state *css = CONTAINER_OF(dev->cds, *css, cds);

    colo_nic_teardown(egc, dev, primary, css->colo_agent_script);
}

const libxl__checkpoint_device_instance_ops colo_save_device_nic = {
    .kind = LIBXL__DEVICE_KIND_VIF,
    .setup = colo_nic_save_setup,
    .teardown = colo_nic_save_teardown,
};

/* ======== secondary ======== */
static void colo_nic_restore_setup(libxl__egc *egc,
                                   libxl__checkpoint_device *dev)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(dev->cds, *crs, cds);

    colo_nic_setup(egc, dev, secondary, crs->colo_agent_script);
}

static void colo_nic_restore_teardown(libxl__egc *egc,
                                      libxl__checkpoint_device *dev)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(dev->cds, *crs, cds);

    colo_nic_teardown(egc, dev, secondary, crs->colo_agent_script);
}

const libxl__checkpoint_device_instance_ops colo_restore_device_nic = {
    .kind = LIBXL__DEVICE_KIND_VIF,
    .setup = colo_nic_restore_setup,
    .teardown = colo_nic_restore_teardown,
};
