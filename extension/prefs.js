/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 *
 * LibrePods Preferences Window
 */

import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';
import Gdk from 'gi://Gdk';
import Adw from 'gi://Adw';
import GObject from 'gi://GObject';

import {ExtensionPreferences, gettext as _} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

import {AirPodsInterface, BUS_NAME, OBJECT_PATH} from './dbusInterface.js';

const AirPodsProxy = Gio.DBusProxy.makeProxyWrapper(AirPodsInterface);

/* Modal window capturing a keyboard shortcut */
const ShortcutDialog = GObject.registerClass(
class ShortcutDialog extends Adw.Window {
    _init(parent, onCaptured) {
        super._init({
            transient_for: parent,
            modal: true,
            resizable: false,
            title: _('Set Shortcut'),
            default_width: 360,
            default_height: 200,
        });

        this._onCaptured = onCaptured;

        const page = new Adw.StatusPage({
            icon_name: 'preferences-desktop-keyboard-shortcuts-symbolic',
            title: _('Press a key combination'),
            description: _('Press Esc to cancel or Backspace to disable the shortcut'),
        });
        this.set_content(page);

        const controller = new Gtk.EventControllerKey();
        this.add_controller(controller);
        controller.connect('key-pressed', (ctrl, keyval, keycode, state) => {
            const mask = state & Gtk.accelerator_get_default_mod_mask();

            if (mask === 0 && keyval === Gdk.KEY_Escape) {
                this.close();
                return Gdk.EVENT_STOP;
            }

            if (mask === 0 && keyval === Gdk.KEY_BackSpace) {
                this._onCaptured('');
                this.close();
                return Gdk.EVENT_STOP;
            }

            if (!Gtk.accelerator_valid(keyval, mask))
                return Gdk.EVENT_STOP;

            this._onCaptured(Gtk.accelerator_name(keyval, mask));
            this.close();
            return Gdk.EVENT_STOP;
        });
    }
});

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
            title: _('Device Status'),
            description: _('Current AirPods connection status'),
        });
        page.add(statusGroup);

        this._statusRow = new Adw.ActionRow({
            title: _('Connection'),
            subtitle: _('Checking…'),
            icon_name: 'bluetooth-active-symbolic',
        });
        statusGroup.add(this._statusRow);

        this._earDetectionRow = new Adw.ActionRow({
            title: _('Ear Detection'),
            subtitle: _('Unknown'),
            icon_name: 'audio-headphones-symbolic',
        });
        statusGroup.add(this._earDetectionRow);

        /* Device Profile group */
        const profileGroup = new Adw.PreferencesGroup({
            title: _('Device Profile'),
            description: _('Customize your AirPods. Leave the name empty to use the device model name'),
        });
        page.add(profileGroup);

        /* Custom name entry */
        this._displayNameRow = new Adw.EntryRow({
            title: _('Custom Name'),
            show_apply_button: true,
        });
        profileGroup.add(this._displayNameRow);

        /* Features group */
        const featuresGroup = new Adw.PreferencesGroup({
            title: _('AirPods Features'),
            description: _('Advanced features for your AirPods'),
        });
        page.add(featuresGroup);

        /* Conversational Awareness */
        this._caRow = new Adw.SwitchRow({
            title: _('Conversational Awareness'),
            subtitle: _('Automatically lower volume when you speak'),
        });
        featuresGroup.add(this._caRow);

        /* Adaptive Noise Level */
        this._adaptiveRow = new Adw.SpinRow({
            title: _('Adaptive Noise Level'),
            subtitle: _('Adjust the transparency level (0-100)'),
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
            title: _('Long Press Actions'),
            description: _('Configure which modes are cycled when pressing and holding the stem'),
        });
        page.add(longPressGroup);

        this._lpOffRow = new Adw.SwitchRow({
            title: _('Off'),
            subtitle: _('Include Off mode in long press cycle'),
        });
        longPressGroup.add(this._lpOffRow);

        this._lpTransparencyRow = new Adw.SwitchRow({
            title: _('Transparency'),
            subtitle: _('Include Transparency mode in long press cycle'),
        });
        longPressGroup.add(this._lpTransparencyRow);

        this._lpANCRow = new Adw.SwitchRow({
            title: _('Noise Cancellation'),
            subtitle: _('Include ANC mode in long press cycle'),
        });
        longPressGroup.add(this._lpANCRow);

        this._lpAdaptiveRow = new Adw.SwitchRow({
            title: _('Adaptive'),
            subtitle: _('Include Adaptive mode in long press cycle'),
        });
        longPressGroup.add(this._lpAdaptiveRow);

        /* Media Control group */
        const mediaGroup = new Adw.PreferencesGroup({
            title: _('Media Control'),
            description: _('Configure media playback behavior'),
        });
        page.add(mediaGroup);

        const pauseModeModel = new Gtk.StringList();
        pauseModeModel.append(_('Disabled'));
        pauseModeModel.append(_('When one earbud removed'));
        pauseModeModel.append(_('When both earbuds removed'));

        this._earPauseRow = new Adw.ComboRow({
            title: _('Auto-pause media'),
            subtitle: _('Pause playback when earbuds are removed'),
            model: pauseModeModel,
        });
        mediaGroup.add(this._earPauseRow);

        /* Notifications group */
        const notificationsGroup = new Adw.PreferencesGroup({
            title: _('Notifications'),
            description: _('Configure notification preferences'),
        });
        page.add(notificationsGroup);

        this._connectionNotifRow = new Adw.SwitchRow({
            title: _('Connection notifications'),
            subtitle: _('Notify when AirPods connect or disconnect'),
        });
        notificationsGroup.add(this._connectionNotifRow);

        this._batteryNotifRow = new Adw.SwitchRow({
            title: _('Low battery notifications'),
            subtitle: _('Notify when battery drops below threshold'),
        });
        notificationsGroup.add(this._batteryNotifRow);

        this._batteryThresholdRow = new Adw.SpinRow({
            title: _('Low battery threshold'),
            subtitle: _('Notify when battery drops below this percentage'),
            adjustment: new Gtk.Adjustment({
                lower: 5,
                upper: 50,
                step_increment: 5,
                page_increment: 10,
            }),
        });
        notificationsGroup.add(this._batteryThresholdRow);

        /* Shortcuts group */
        const shortcutsGroup = new Adw.PreferencesGroup({
            title: _('Keyboard Shortcuts'),
        });
        page.add(shortcutsGroup);

        this._shortcutLabel = new Gtk.ShortcutLabel({
            disabled_text: _('Disabled'),
            valign: Gtk.Align.CENTER,
        });

        const shortcutRow = new Adw.ActionRow({
            title: _('Cycle noise control mode'),
            subtitle: _('Also works when the Quick Settings menu is closed'),
            activatable: true,
        });
        shortcutRow.add_suffix(this._shortcutLabel);
        shortcutRow.connect('activated', () => {
            const dialog = new ShortcutDialog(window, accel => {
                this._settings.set_strv('cycle-noise-mode-shortcut', accel ? [accel] : []);
                this._shortcutLabel.accelerator = accel;
            });
            dialog.present();
        });
        shortcutsGroup.add(shortcutRow);

        /* About group */
        const aboutGroup = new Adw.PreferencesGroup({
            title: _('About'),
        });
        page.add(aboutGroup);

        const aboutRow = new Adw.ActionRow({
            title: 'LibrePods',
            subtitle: _('AirPods integration for GNOME'),
            icon_name: 'audio-headphones-symbolic',
        });
        aboutGroup.add(aboutRow);

        const versionRow = new Adw.ActionRow({
            title: _('Version'),
            subtitle: this.metadata['version-name'] ?? String(this.metadata.version),
            icon_name: 'dialog-information-symbolic',
        });
        aboutGroup.add(versionRow);

        /* Load settings */
        this._settings = this.getSettings();
        this._connectionNotifRow.active = this._settings.get_boolean('enable-connection-notifications');
        this._batteryNotifRow.active = this._settings.get_boolean('enable-low-battery-notifications');
        this._batteryThresholdRow.value = this._settings.get_int('low-battery-threshold');

        const shortcuts = this._settings.get_strv('cycle-noise-mode-shortcut');
        this._shortcutLabel.accelerator = shortcuts.length > 0 ? shortcuts[0] : '';

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
            if (this._proxy?.Connected) {
                this._proxy.SetConversationalAwarenessRemote(this._caRow.active, () => {});
            }
        });

        this._adaptiveRow.connect('notify::value', () => {
            if (this._updatingFromProxy)
                return;
            if (this._proxy?.Connected) {
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
            if (this._proxy?.Connected && !this._updatingListeningModes) {
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
            if (this._proxy?.Connected) {
                const newName = this._displayNameRow.text.trim();
                this._proxy.SetDisplayNameRemote(newName, () => {});
            }
        });
    }

    _connectProxy() {
        try {
            this._proxy = new AirPodsProxy(
                Gio.DBus.session,
                BUS_NAME,
                OBJECT_PATH,
                (proxy, error) => {
                    if (error) {
                        this._statusRow.subtitle = _('Daemon not running');
                        this._setSensitive(false);
                        return;
                    }

                    this._onProxyReady();
                }
            );
        } catch (e) {
            this._statusRow.subtitle = _('Error connecting to daemon');
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
            this._statusRow.subtitle = _('Connected: %s').replace('%s', displayName);

            const leftEar = this._proxy.LeftInEar ? _('In ear') : _('Out');
            const rightEar = this._proxy.RightInEar ? _('In ear') : _('Out');
            this._earDetectionRow.subtitle = `${_('Left')}: ${leftEar}, ${_('Right')}: ${rightEar}`;

            /* Update display name entry - show custom name or empty for model default */
            const modelName = this._proxy.DeviceModel || 'AirPods';
            if (this._proxy.DisplayName && this._proxy.DisplayName !== modelName) {
                this._displayNameRow.text = this._proxy.DisplayName;
            } else {
                this._displayNameRow.text = '';
            }
            this._displayNameRow.set_tooltip_text(
                _('Device model: %s').replace('%s', modelName));

            this._caRow.active = this._proxy.ConversationalAwareness;
            this._adaptiveRow.value = this._proxy.AdaptiveNoiseLevel;
            this._earPauseRow.selected = this._proxy.EarPauseMode;

            this._lpOffRow.active = this._proxy.ListeningModeOff;
            this._lpTransparencyRow.active = this._proxy.ListeningModeTransparency;
            this._lpANCRow.active = this._proxy.ListeningModeANC;
            this._lpAdaptiveRow.active = this._proxy.ListeningModeAdaptive;

            this._setSensitive(true);
        } else {
            this._statusRow.subtitle = _('Disconnected');
            this._earDetectionRow.subtitle = _('No device connected');
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
