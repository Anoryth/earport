/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "fake_bluez.h"
#include "bluetooth.h"

#include <string.h>

#define DEVICE_INTERFACE "org.bluez.Device1"
#define ADAPTER_INTERFACE "org.bluez.Adapter1"

static const char bluez_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.DBus.ObjectManager'>"
    "    <method name='GetManagedObjects'>"
    "      <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>"
    "    </method>"
    "    <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>"
    "    <signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>"
    "  </interface>"
    "  <interface name='org.bluez.Adapter1'>"
    "    <method name='SetDiscoveryFilter'><arg type='a{sv}' direction='in'/></method>"
    "    <method name='StartDiscovery'/>"
    "    <method name='StopDiscovery'/>"
    "    <property name='Address' type='s' access='read'/>"
    "  </interface>"
    "  <interface name='org.bluez.ProfileManager1'>"
    "    <method name='RegisterProfile'>"
    "      <arg type='o' direction='in'/><arg type='s' direction='in'/>"
    "      <arg type='a{sv}' direction='in'/>"
    "    </method>"
    "    <method name='UnregisterProfile'><arg type='o' direction='in'/></method>"
    "  </interface>"
    "  <interface name='org.bluez.BatteryProviderManager1'>"
    "    <method name='RegisterBatteryProvider'><arg type='o' direction='in'/></method>"
    "    <method name='UnregisterBatteryProvider'><arg type='o' direction='in'/></method>"
    "  </interface>"
    "  <interface name='org.bluez.Device1'>"
    "    <method name='Connect'/>"
    "    <method name='Disconnect'/>"
    "    <property name='Address' type='s' access='read'/>"
    "    <property name='Name' type='s' access='read'/>"
    "    <property name='Connected' type='b' access='read'/>"
    "    <property name='Paired' type='b' access='read'/>"
    "    <property name='UUIDs' type='as' access='read'/>"
    "    <property name='RSSI' type='n' access='read'/>"
    "    <property name='ManufacturerData' type='a{qv}' access='read'/>"
    "  </interface>"
    "</node>";

typedef struct {
    char *path;
    GHashTable *props;          /* Device1 or Adapter1 property -> GVariant */
    guint registration_id;
} FakeObject;

typedef struct {
    char *method;
    GVariant *params;
} Call;

static struct {
    GTestDBus *bus;
    GThread *thread;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *connection;
    GDBusNodeInfo *info;
    guint owner_id;

    GMutex lock;                /* Everything below */
    GCond cond;
    bool ready;
    GHashTable *objects;        /* path -> FakeObject */
    GPtrArray *calls;           /* Call */
    GHashTable *failing;        /* method names */
    GHashTable *registrations;  /* "kind sender path": battery providers, profiles */
    guint get_delay_ms;         /* Properties.Get answered this late */
} fake;

static void object_free(gpointer data)
{
    FakeObject *obj = data;
    g_free(obj->path);
    g_hash_table_destroy(obj->props);
    g_free(obj);
}

static FakeObject *object_new(const char *path)
{
    FakeObject *obj = g_new0(FakeObject, 1);
    obj->path = g_strdup(path);
    obj->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)g_variant_unref);
    return obj;
}

static void set_prop(FakeObject *obj, const char *name, GVariant *value)
{
    g_hash_table_insert(obj->props, g_strdup(name), g_variant_ref_sink(value));
}

static void record_call(const char *method, GVariant *params)
{
    Call *call = g_new0(Call, 1);
    call->method = g_strdup(method);
    call->params = g_variant_ref_sink(params);
    g_ptr_array_add(fake.calls, call);
}

static void call_free(gpointer data)
{
    Call *call = data;
    g_free(call->method);
    g_variant_unref(call->params);
    g_free(call);
}

/* Runs fn in the fake's thread and waits for it: objects are registered
 * there, so that their calls are answered there */
typedef struct {
    void (*fn)(gpointer);
    gpointer data;
    bool done;
} Invoke;

static gboolean invoke_cb(gpointer user_data)
{
    Invoke *invoke = user_data;
    invoke->fn(invoke->data);
    g_mutex_lock(&fake.lock);
    invoke->done = true;
    g_cond_broadcast(&fake.cond);
    g_mutex_unlock(&fake.lock);
    return G_SOURCE_REMOVE;
}

static void invoke_sync(void (*fn)(gpointer), gpointer data)
{
    Invoke invoke = { fn, data, false };
    g_main_context_invoke(fake.context, invoke_cb, &invoke);
    g_mutex_lock(&fake.lock);
    while (!invoke.done)
        g_cond_wait(&fake.cond, &fake.lock);
    g_mutex_unlock(&fake.lock);
}

/* ============================================================================
 * D-Bus side, in the fake's thread
 * ========================================================================== */

static GVariant *props_variant(FakeObject *obj)
{
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    GHashTableIter iter;
    gpointer name, value;
    g_hash_table_iter_init(&iter, obj->props);
    while (g_hash_table_iter_next(&iter, &name, &value))
        g_variant_builder_add(&props, "{sv}", name, value);
    return g_variant_builder_end(&props);
}

static GVariant *get_property(GDBusConnection *connection, const char *sender,
                              const char *object_path, const char *interface_name,
                              const char *property_name, GError **error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)interface_name;
    (void)user_data;
    g_mutex_lock(&fake.lock);
    FakeObject *obj = g_hash_table_lookup(fake.objects, object_path);
    GVariant *value = obj ? g_hash_table_lookup(obj->props, property_name) : NULL;
    if (value != NULL)
        g_variant_ref(value);
    /* Only Get passes an error: counted as "Get" */
    if (error != NULL)
        record_call("Get", g_variant_new("(ss)", object_path, property_name));
    guint delay_ms = error != NULL ? fake.get_delay_ms : 0;
    g_mutex_unlock(&fake.lock);
    if (delay_ms > 0)
        g_usleep(delay_ms * 1000);

    /* GetAll passes no error and skips the missing ones */
    if (value == NULL && error != NULL)
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "No such property '%s'",
                    property_name);
    return value;
}

static GVariant *managed_objects(void)
{
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    g_mutex_lock(&fake.lock);
    GHashTableIter iter;
    gpointer path, value;
    g_hash_table_iter_init(&iter, fake.objects);
    while (g_hash_table_iter_next(&iter, &path, &value)) {
        bool adapter = g_str_equal(path, FAKE_ADAPTER_PATH);
        g_variant_builder_open(&objects, G_VARIANT_TYPE("{oa{sa{sv}}}"));
        g_variant_builder_add(&objects, "o", path);
        g_variant_builder_open(&objects, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&objects, "{s@a{sv}}", adapter ? ADAPTER_INTERFACE : DEVICE_INTERFACE,
                              props_variant(value));
        g_variant_builder_close(&objects);
        g_variant_builder_close(&objects);
    }
    g_mutex_unlock(&fake.lock);
    return g_variant_new("(a{oa{sa{sv}}})", &objects);
}

static void method_call(GDBusConnection *connection, const char *sender, const char *object_path,
                        const char *interface_name, const char *method_name, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_str_equal(method_name, "GetManagedObjects")) {
        g_dbus_method_invocation_return_value(invocation, managed_objects());
        return;
    }

    g_mutex_lock(&fake.lock);
    record_call(method_name, parameters);
    const char *error = g_hash_table_contains(fake.failing, method_name) ? "org.bluez.Error.Failed"
                                                                         : NULL;
    /* One per kind, sender and path, as BlueZ does */
    const char *kind = g_str_has_prefix(method_name, "Register")     ? method_name + 8
                       : g_str_has_prefix(method_name, "Unregister") ? method_name + 10
                                                                     : NULL;
    if (error == NULL && kind != NULL) {
        const char *path;
        g_variant_get_child(parameters, 0, "&o", &path);
        char *key = g_strdup_printf("%s %s %s", kind, sender, path);
        if (method_name[0] == 'R') {
            if (!g_hash_table_add(fake.registrations, key))
                error = "org.bluez.Error.AlreadyExists";
        } else {
            if (!g_hash_table_remove(fake.registrations, key))
                error = "org.bluez.Error.DoesNotExist";
            g_free(key);
        }
    }
    g_mutex_unlock(&fake.lock);

    if (error != NULL)
        g_dbus_method_invocation_return_dbus_error(invocation, error, "Failed (fake)");
    else
        g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { method_call, get_property, NULL, { 0 } };

static void register_interface(const char *path, const char *interface, guint *id)
{
    GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface(fake.info, interface);
    guint registration = g_dbus_connection_register_object(fake.connection, path, info, &vtable,
                                                           NULL, NULL, NULL);
    g_assert_cmpuint(registration, >, 0);
    if (id != NULL)
        *id = registration;
}

static void register_device(gpointer data)
{
    FakeObject *obj = data;
    register_interface(obj->path, DEVICE_INTERFACE, &obj->registration_id);
}

static void unregister_device(gpointer data)
{
    g_dbus_connection_unregister_object(fake.connection, GPOINTER_TO_UINT(data));
}

static void on_name_acquired(GDBusConnection *connection, const char *name, gpointer user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
    g_mutex_lock(&fake.lock);
    fake.ready = true;
    g_cond_broadcast(&fake.cond);
    g_mutex_unlock(&fake.lock);
}

static void own_name(gpointer data)
{
    (void)data;
    fake.owner_id = g_bus_own_name_on_connection(fake.connection, "org.bluez",
                                                 G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired,
                                                 NULL, NULL, NULL);
}

static void restart(gpointer data)
{
    (void)data;
    g_bus_unown_name(fake.owner_id);
    g_mutex_lock(&fake.lock);
    g_hash_table_remove_all(fake.registrations);
    g_mutex_unlock(&fake.lock);
    own_name(NULL);
}

static gpointer fake_thread(gpointer data)
{
    (void)data;
    g_main_context_push_thread_default(fake.context);

    fake.connection = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(fake.bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_assert_nonnull(fake.connection);

    register_interface("/", "org.freedesktop.DBus.ObjectManager", NULL);
    register_interface(FAKE_ADAPTER_PATH, ADAPTER_INTERFACE, NULL);
    register_interface(FAKE_ADAPTER_PATH, "org.bluez.BatteryProviderManager1", NULL);
    register_interface("/org/bluez", "org.bluez.ProfileManager1", NULL);
    own_name(NULL);

    g_main_loop_run(fake.loop);

    g_bus_unown_name(fake.owner_id);
    g_dbus_connection_flush_sync(fake.connection, NULL, NULL);
    g_object_unref(fake.connection);
    g_main_context_pop_thread_default(fake.context);
    return NULL;
}

/* ============================================================================
 * Test side
 * ========================================================================== */

void fake_bluez_start(void)
{
    fake.bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(fake.bus);
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(fake.bus), TRUE);

    g_mutex_init(&fake.lock);
    g_cond_init(&fake.cond);
    fake.info = g_dbus_node_info_new_for_xml(bluez_xml, NULL);
    fake.objects = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, object_free);
    fake.calls = g_ptr_array_new_with_free_func(call_free);
    fake.failing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    fake.registrations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    FakeObject *adapter = object_new(FAKE_ADAPTER_PATH);
    set_prop(adapter, "Address", g_variant_new_string("00:00:00:00:00:01"));
    g_hash_table_insert(fake.objects, adapter->path, adapter);

    fake.context = g_main_context_new();
    fake.loop = g_main_loop_new(fake.context, FALSE);
    fake.thread = g_thread_new("fake-bluez", fake_thread, NULL);

    g_mutex_lock(&fake.lock);
    while (!fake.ready)
        g_cond_wait(&fake.cond, &fake.lock);
    g_mutex_unlock(&fake.lock);
}

void fake_bluez_stop(void)
{
    fake_bluez_reset();
    g_main_loop_quit(fake.loop);
    g_thread_join(fake.thread);
    g_main_loop_unref(fake.loop);
    g_main_context_unref(fake.context);
    g_hash_table_destroy(fake.objects);
    g_ptr_array_unref(fake.calls);
    g_hash_table_destroy(fake.failing);
    g_hash_table_destroy(fake.registrations);
    g_dbus_node_info_unref(fake.info);
    g_test_dbus_down(fake.bus);
    g_object_unref(fake.bus);
}

void fake_bluez_reset(void)
{
    /* Calls still in flight from the last test land before */
    fake_bluez_sync();
    GHashTableIter iter;
    gpointer path, value;
    g_hash_table_iter_init(&iter, fake.objects);
    while (g_hash_table_iter_next(&iter, &path, &value)) {
        FakeObject *obj = value;
        if (obj->registration_id == 0)
            continue;
        invoke_sync(unregister_device, GUINT_TO_POINTER(obj->registration_id));
        g_mutex_lock(&fake.lock);
        g_hash_table_iter_remove(&iter);
        g_mutex_unlock(&fake.lock);
    }
    g_mutex_lock(&fake.lock);
    g_ptr_array_set_size(fake.calls, 0);
    g_hash_table_remove_all(fake.failing);
    g_hash_table_remove_all(fake.registrations);
    fake.get_delay_ms = 0;
    g_mutex_unlock(&fake.lock);
}

const char *fake_bluez_add_device(const char *address, const char *name, bool airpods,
                                  bool connected, bool announce)
{
    g_autofree char *device = g_strdup(address);
    g_strdelimit(device, ":", '_');
    g_autofree char *path = g_strdup_printf("%s/dev_%s", FAKE_ADAPTER_PATH, device);
    FakeObject *obj = object_new(path);

    const char *uuids[] = { "0000110b-0000-1000-8000-00805f9b34fb", airpods ? AIRPODS_UUID : NULL,
                            NULL };
    set_prop(obj, "Address", g_variant_new_string(address));
    set_prop(obj, "Name", g_variant_new_string(name));
    set_prop(obj, "Connected", g_variant_new_boolean(connected));
    set_prop(obj, "Paired", g_variant_new_boolean(TRUE));
    set_prop(obj, "UUIDs", g_variant_new_strv(uuids, -1));

    g_mutex_lock(&fake.lock);
    g_hash_table_insert(fake.objects, obj->path, obj);
    g_mutex_unlock(&fake.lock);
    invoke_sync(register_device, obj);

    if (announce)
        fake_bluez_announce(obj->path);
    return obj->path;
}

void fake_bluez_announce(const char *path)
{
    GVariantBuilder interfaces;
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_mutex_lock(&fake.lock);
    FakeObject *obj = g_hash_table_lookup(fake.objects, path);
    g_assert_nonnull(obj);
    g_variant_builder_add(&interfaces, "{s@a{sv}}", DEVICE_INTERFACE, props_variant(obj));
    g_mutex_unlock(&fake.lock);
    g_dbus_connection_emit_signal(fake.connection, NULL, "/", "org.freedesktop.DBus.ObjectManager",
                                  "InterfacesAdded",
                                  g_variant_new("(oa{sa{sv}})", path, &interfaces), NULL);
}

void fake_bluez_set(const char *path, const char *property, GVariant *value, bool emit)
{
    g_variant_ref_sink(value);
    g_mutex_lock(&fake.lock);
    FakeObject *obj = g_hash_table_lookup(fake.objects, path);
    g_assert_nonnull(obj);
    set_prop(obj, property, value);   /* Already sunk: takes a reference */
    g_mutex_unlock(&fake.lock);

    if (emit) {
        GVariantBuilder changed;
        g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&changed, "{sv}", property, value);
        g_dbus_connection_emit_signal(fake.connection, NULL, path,
                                      "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)", DEVICE_INTERFACE, &changed, NULL),
                                      NULL);
    }
    g_variant_unref(value);
}

void fake_bluez_remove_interfaces(const char *path, const char *const *interfaces)
{
    g_dbus_connection_emit_signal(fake.connection, NULL, "/", "org.freedesktop.DBus.ObjectManager",
                                  "InterfacesRemoved", g_variant_new("(o^as)", path, interfaces),
                                  NULL);
    if (!g_strv_contains(interfaces, DEVICE_INTERFACE))
        return;

    g_mutex_lock(&fake.lock);
    FakeObject *obj = g_hash_table_lookup(fake.objects, path);
    guint id = obj ? obj->registration_id : 0;
    g_mutex_unlock(&fake.lock);
    if (id == 0)
        return;
    invoke_sync(unregister_device, GUINT_TO_POINTER(id));
    g_mutex_lock(&fake.lock);
    g_hash_table_remove(fake.objects, path);
    g_mutex_unlock(&fake.lock);
}

int fake_bluez_calls(const char *method)
{
    int count = 0;
    g_mutex_lock(&fake.lock);
    for (guint i = 0; i < fake.calls->len; i++)
        if (g_str_equal(((Call *)fake.calls->pdata[i])->method, method))
            count++;
    g_mutex_unlock(&fake.lock);
    return count;
}

GVariant *fake_bluez_last_call(const char *method)
{
    GVariant *params = NULL;
    g_mutex_lock(&fake.lock);
    for (guint i = fake.calls->len; i > 0 && params == NULL; i--) {
        Call *call = fake.calls->pdata[i - 1];
        if (g_str_equal(call->method, method))
            params = g_variant_ref(call->params);
    }
    g_mutex_unlock(&fake.lock);
    return params;
}

int fake_bluez_wait_calls(const char *method, int count)
{
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (fake_bluez_calls(method) < count && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    return fake_bluez_calls(method);
}

void fake_bluez_fail(const char *method, bool fail)
{
    g_mutex_lock(&fake.lock);
    if (fail)
        g_hash_table_add(fake.failing, g_strdup(method));
    else
        g_hash_table_remove(fake.failing, method);
    g_mutex_unlock(&fake.lock);
}

int fake_bluez_registered(const char *kind)
{
    g_autofree char *prefix = g_strdup_printf("%s ", kind);
    int count = 0;
    g_mutex_lock(&fake.lock);
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, fake.registrations);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        if (g_str_has_prefix(key, prefix))
            count++;
    g_mutex_unlock(&fake.lock);
    return count;
}

int fake_bluez_wait_registered(const char *kind, int count)
{
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (fake_bluez_registered(kind) != count && g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
    return fake_bluez_registered(kind);
}

void fake_bluez_emit(const char *path, const char *interface, const char *signal,
                     GVariant *parameters)
{
    g_dbus_connection_emit_signal(fake.connection, NULL, path, interface, signal, parameters,
                                  NULL);
}

void fake_bluez_restart(void)
{
    invoke_sync(restart, NULL);
}

void fake_bluez_sync(void)
{
    /* Through the bus daemon (match rules applied) to the fake's thread
     * (earlier calls dispatched): both in order */
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.bluez", "/",
                                                  "org.freedesktop.DBus.ObjectManager",
                                                  "GetManagedObjects", NULL, NULL,
                                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    g_clear_pointer(&reply, g_variant_unref);
    g_object_unref(bus);
}

void fake_bluez_settle(void)
{
    /* The answers to the calls sent before are queued when this one
     * returns: dispatch them */
    fake_bluez_sync();
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
}

void fake_bluez_delay_get(guint ms)
{
    g_mutex_lock(&fake.lock);
    fake.get_delay_ms = ms;
    g_mutex_unlock(&fake.lock);
}

void fake_bluez_run(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(NULL, FALSE);
}
