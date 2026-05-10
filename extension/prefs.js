/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 *
 * LibrePods Preferences Window
 */

import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';
import Adw from 'gi://Adw';
import GObject from 'gi://GObject';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

/* D-Bus interface */
const AirPodsInterface = `
<node>
  <interface name="org.librepods.AirPods1">
    <property name="Connected" type="b" access="read"/>
    <property name="DeviceName" type="s" access="read"/>
    <property name="DeviceModel" type="s" access="read"/>
    <property name="DisplayName" type="s" access="read"/>
    <property name="ConversationalAwareness" type="b" access="read"/>
    <property name="AdaptiveNoiseLevel" type="i" access="read"/>
    <property name="LeftInEar" type="b" access="read"/>
    <property name="RightInEar" type="b" access="read"/>
    <property name="EarPauseMode" type="i" access="read"/>
    <property name="ListeningModeOff" type="b" access="read"/>
    <property name="ListeningModeTransparency" type="b" access="read"/>
    <property name="ListeningModeANC" type="b" access="read"/>
    <property name="ListeningModeAdaptive" type="b" access="read"/>
    <method name="SetConversationalAwareness">
      <arg type="b" name="enabled" direction="in"/>
    </method>
    <method name="SetAdaptiveNoiseLevel">
      <arg type="i" name="level" direction="in"/>
    </method>
    <method name="SetEarPauseMode">
      <arg type="i" name="mode" direction="in"/>
    </method>
    <method name="SetListeningModes">
      <arg type="b" name="off" direction="in"/>
      <arg type="b" name="transparency" direction="in"/>
      <arg type="b" name="anc" direction="in"/>
      <arg type="b" name="adaptive" direction="in"/>
    </method>
    <method name="SetDisplayName">
      <arg type="s" name="name" direction="in"/>
    </method>
  </interface>
</node>
`;

const AirPodsProxy = Gio.DBusProxy.makeProxyWrapper(AirPodsInterface);

export default class LibrePodsPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        this._proxy = null;
        this._propertiesChangedId = 0;

        /* Cleanup proxy when window is closed */
        window.connect('close-request', () => {
            this._disconnectProxy();
        });

        const page = new Adw.PreferencesPage({
            title: 'LibrePods',
            icon_name: 'audio-headphones-symbolic',
        });
        window.add(page);

        /* Device status group */
        const statusGroup = new Adw.PreferencesGroup({
            title: 'Device Status',
            description: 'Current AirPods connection status',
        });
        page.add(statusGroup);

        this._statusRow = new Adw.ActionRow({
            title: 'Connection',
            subtitle: 'Checking...',
            icon_name: 'bluetooth-active-symbolic',
        });
        statusGroup.add(this._statusRow);

        this._earDetectionRow = new Adw.ActionRow({
            title: 'Ear Detection',
            subtitle: 'Unknown',
            icon_name: 'audio-headphones-symbolic',
        });
        statusGroup.add(this._earDetectionRow);

        /* Device Profile group */
        const profileGroup = new Adw.PreferencesGroup({
            title: 'Device Profile',
            description: 'Customize your AirPods settings',
        });
        page.add(profileGroup);

        /* Custom name entry */
        this._displayNameRow = new Adw.EntryRow({
            title: 'Custom Name',
            show_apply_button: true,
        });
        profileGroup.add(this._displayNameRow);

        /* Hint row */
        const nameHintRow = new Adw.ActionRow({
            title: '',
            subtitle: 'Leave empty to use the device model name',
        });
        nameHintRow.add_css_class('dim-label');
        profileGroup.add(nameHintRow);

        /* Features group */
        const featuresGroup = new Adw.PreferencesGroup({
            title: 'AirPods Features',
            description: 'Advanced features for your AirPods',
        });
        page.add(featuresGroup);

        /* Conversational Awareness - using Adw.SwitchRow */
        this._caRow = new Adw.SwitchRow({
            title: 'Conversational Awareness',
            subtitle: 'Automatically lower volume when you speak',
        });
        featuresGroup.add(this._caRow);

        /* Adaptive Noise Level - using Adw.SpinRow */
        this._adaptiveRow = new Adw.SpinRow({
            title: 'Adaptive Noise Level',
            subtitle: 'Adjust the transparency level (0-100)',
            adjustment: new Gtk.Adjustment({
                lower: 0,
                upper: 100,
                step_increment: 5,
                page_increment: 10,
            }),
        });
        featuresGroup.add(this._adaptiveRow);

        /* Long Press Actions group */
        const longPressGroup = new Adw.PreferencesGroup({
            title: 'Long Press Actions',
            description: 'Configure which modes are cycled when pressing and holding the stem',
        });
        page.add(longPressGroup);

        /* Off mode */
        this._lpOffRow = new Adw.SwitchRow({
            title: 'Off',
            subtitle: 'Include Off mode in long press cycle',
        });
        longPressGroup.add(this._lpOffRow);

        /* Transparency mode */
        this._lpTransparencyRow = new Adw.SwitchRow({
            title: 'Transparency',
            subtitle: 'Include Transparency mode in long press cycle',
        });
        longPressGroup.add(this._lpTransparencyRow);

        /* ANC mode */
        this._lpANCRow = new Adw.SwitchRow({
            title: 'Noise Cancellation',
            subtitle: 'Include ANC mode in long press cycle',
        });
        longPressGroup.add(this._lpANCRow);

        /* Adaptive mode */
        this._lpAdaptiveRow = new Adw.SwitchRow({
            title: 'Adaptive',
            subtitle: 'Include Adaptive mode in long press cycle',
        });
        longPressGroup.add(this._lpAdaptiveRow);

        /* Media Control group */
        const mediaGroup = new Adw.PreferencesGroup({
            title: 'Media Control',
            description: 'Configure media playback behavior',
        });
        page.add(mediaGroup);

        /* Ear pause mode - using Adw.ComboRow */
        const pauseModeModel = new Gtk.StringList();
        pauseModeModel.append('Disabled');
        pauseModeModel.append('When one earbud removed');
        pauseModeModel.append('When both earbuds removed');

        this._earPauseRow = new Adw.ComboRow({
            title: 'Auto-pause media',
            subtitle: 'Pause playback when earbuds are removed',
            model: pauseModeModel,
        });
        mediaGroup.add(this._earPauseRow);

        /* Notifications group */
        const notificationsGroup = new Adw.PreferencesGroup({
            title: 'Notifications',
            description: 'Configure notification preferences',
        });
        page.add(notificationsGroup);

        /* Connection notifications - using Adw.SwitchRow */
        this._connectionNotifRow = new Adw.SwitchRow({
            title: 'Connection notifications',
            subtitle: 'Notify when AirPods connect or disconnect',
        });
        notificationsGroup.add(this._connectionNotifRow);

        /* Low battery notifications - using Adw.SwitchRow */
        this._batteryNotifRow = new Adw.SwitchRow({
            title: 'Low battery notifications',
            subtitle: 'Notify when battery drops below threshold',
        });
        notificationsGroup.add(this._batteryNotifRow);

        /* Battery threshold - using Adw.SpinRow */
        this._batteryThresholdRow = new Adw.SpinRow({
            title: 'Low battery threshold',
            subtitle: 'Notify when battery drops below this percentage',
            adjustment: new Gtk.Adjustment({
                lower: 5,
                upper: 50,
                step_increment: 5,
                page_increment: 10,
            }),
        });
        notificationsGroup.add(this._batteryThresholdRow);

        /* About group */
        const aboutGroup = new Adw.PreferencesGroup({
            title: 'About',
        });
        page.add(aboutGroup);

        const aboutRow = new Adw.ActionRow({
            title: 'LibrePods',
            subtitle: 'AirPods integration for GNOME',
            icon_name: 'audio-headphones-symbolic',
        });
        aboutGroup.add(aboutRow);

        const versionRow = new Adw.ActionRow({
            title: 'Version',
            subtitle: '0.1.0',
            icon_name: 'dialog-information-symbolic',
        });
        aboutGroup.add(versionRow);

        /* Load settings */
        this._settings = this.getSettings();
        this._connectionNotifRow.active = this._settings.get_boolean('enable-connection-notifications');
        this._batteryNotifRow.active = this._settings.get_boolean('enable-low-battery-notifications');
        this._batteryThresholdRow.value = this._settings.get_int('low-battery-threshold');

        /* Connect to daemon */
        this._connectProxy();

        /* Connect settings signals */
        this._connectionNotifRow.connect('notify::active', () => {
            this._settings.set_boolean('enable-connection-notifications', this._connectionNotifRow.active);
        });

        this._batteryNotifRow.connect('notify::active', () => {
            this._settings.set_boolean('enable-low-battery-notifications', this._batteryNotifRow.active);
        });

        this._batteryThresholdRow.connect('notify::value', () => {
            this._settings.set_int('low-battery-threshold', this._batteryThresholdRow.value);
        });

        /* Connect UI signals for daemon settings */
        this._caRow.connect('notify::active', () => {
            if (this._updatingFromProxy)
                return;
            if (this._proxy && this._proxy.Connected) {
                this._proxy.SetConversationalAwarenessRemote(this._caRow.active, () => {});
            }
        });

        this._adaptiveRow.connect('notify::value', () => {
            if (this._updatingFromProxy)
                return;
            if (this._proxy && this._proxy.Connected) {
                this._proxy.SetAdaptiveNoiseLevelRemote(this._adaptiveRow.value, () => {});
            }
        });

        this._earPauseRow.connect('notify::selected', () => {
            if (this._updatingFromProxy)
                return;
            if (this._proxy) {
                this._proxy.SetEarPauseModeRemote(this._earPauseRow.selected, () => {});
            }
        });

        /* Long press modes change handlers */
        const onListeningModeChanged = () => {
            if (this._proxy && this._proxy.Connected && !this._updatingListeningModes) {
                /* Ensure at least 2 modes are enabled */
                const enabledCount = (this._lpOffRow.active ? 1 : 0) +
                                     (this._lpTransparencyRow.active ? 1 : 0) +
                                     (this._lpANCRow.active ? 1 : 0) +
                                     (this._lpAdaptiveRow.active ? 1 : 0);

                if (enabledCount < 2) {
                    /* Revert the change - restore from proxy */
                    this._updatingListeningModes = true;
                    this._lpOffRow.active = this._proxy.ListeningModeOff;
                    this._lpTransparencyRow.active = this._proxy.ListeningModeTransparency;
                    this._lpANCRow.active = this._proxy.ListeningModeANC;
                    this._lpAdaptiveRow.active = this._proxy.ListeningModeAdaptive;
                    this._updatingListeningModes = false;
                    return;
                }

                this._proxy.SetListeningModesRemote(
                    this._lpOffRow.active,
                    this._lpTransparencyRow.active,
                    this._lpANCRow.active,
                    this._lpAdaptiveRow.active,
                    () => {}
                );
            }
        };

        this._lpOffRow.connect('notify::active', onListeningModeChanged);
        this._lpTransparencyRow.connect('notify::active', onListeningModeChanged);
        this._lpANCRow.connect('notify::active', onListeningModeChanged);
        this._lpAdaptiveRow.connect('notify::active', onListeningModeChanged);

        /* Custom name change handler */
        this._displayNameRow.connect('apply', () => {
            if (this._proxy && this._proxy.Connected) {
                const newName = this._displayNameRow.text.trim();
                this._proxy.SetDisplayNameRemote(newName, () => {});
            }
        });
    }

    _connectProxy() {
        try {
            this._proxy = new AirPodsProxy(
                Gio.DBus.session,
                'org.librepods.Daemon',
                '/org/librepods/AirPods',
                (proxy, error) => {
                    if (error) {
                        this._statusRow.subtitle = 'Daemon not running';
                        this._setSensitive(false);
                        return;
                    }

                    this._onProxyReady();
                }
            );
        } catch (e) {
            this._statusRow.subtitle = 'Error connecting to daemon';
            this._setSensitive(false);
        }
    }

    _onProxyReady() {
        this._propertiesChangedId = this._proxy.connect('g-properties-changed', () => {
            this._updateState();
        });

        /* Initialize ear pause mode (available even when AirPods are not connected) */
        this._updatingFromProxy = true;
        this._earPauseRow.selected = this._proxy.EarPauseMode;
        this._updatingFromProxy = false;

        this._updateState();
    }

    _disconnectProxy() {
        if (this._proxy && this._propertiesChangedId) {
            this._proxy.disconnect(this._propertiesChangedId);
            this._propertiesChangedId = 0;
        }
        this._proxy = null;
    }

    _updateState() {
        if (!this._proxy)
            return;

        const connected = this._proxy.Connected;

        /* Suppress feedback loop: programmatic widget updates would otherwise
         * fire notify handlers that push values back to the daemon. */
        this._updatingFromProxy = true;
        this._updatingListeningModes = true;

        if (connected) {
            const displayName = this._proxy.DisplayName || this._proxy.DeviceModel || 'AirPods';
            this._statusRow.subtitle = `Connected: ${displayName}`;

            const leftEar = this._proxy.LeftInEar ? 'In ear' : 'Out';
            const rightEar = this._proxy.RightInEar ? 'In ear' : 'Out';
            this._earDetectionRow.subtitle = `Left: ${leftEar}, Right: ${rightEar}`;

            /* Update display name entry - show custom name or empty for model default */
            const modelName = this._proxy.DeviceModel || 'AirPods';
            if (this._proxy.DisplayName && this._proxy.DisplayName !== modelName) {
                this._displayNameRow.text = this._proxy.DisplayName;
            } else {
                this._displayNameRow.text = '';
            }
            this._displayNameRow.set_tooltip_text(`Device model: ${modelName}`);

            this._caRow.active = this._proxy.ConversationalAwareness;
            this._adaptiveRow.value = this._proxy.AdaptiveNoiseLevel;
            this._earPauseRow.selected = this._proxy.EarPauseMode;

            this._lpOffRow.active = this._proxy.ListeningModeOff;
            this._lpTransparencyRow.active = this._proxy.ListeningModeTransparency;
            this._lpANCRow.active = this._proxy.ListeningModeANC;
            this._lpAdaptiveRow.active = this._proxy.ListeningModeAdaptive;

            this._setSensitive(true);
        } else {
            this._statusRow.subtitle = 'Disconnected';
            this._earDetectionRow.subtitle = 'No device connected';
            this._displayNameRow.text = '';
            this._setSensitive(false);
        }

        this._updatingFromProxy = false;
        this._updatingListeningModes = false;
    }

    _setSensitive(sensitive) {
        this._caRow.sensitive = sensitive;
        this._adaptiveRow.sensitive = sensitive;
        this._displayNameRow.sensitive = sensitive;
        /* Listening modes require AirPods connection */
        this._lpOffRow.sensitive = sensitive;
        this._lpTransparencyRow.sensitive = sensitive;
        this._lpANCRow.sensitive = sensitive;
        this._lpAdaptiveRow.sensitive = sensitive;
        /* Ear pause mode is always available (doesn't require AirPods connection) */
    }
}
