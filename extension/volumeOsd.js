/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Volume OSD when the volume changes from the AirPods (stem swipe or
 * Digital Crown): Bluetooth absolute volume moves the sink, but only the
 * keyboard keys make GNOME show the OSD.
 */

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Volume from 'resource:///org/gnome/shell/ui/status/volume.js';

import {getAirPodsSink} from './conversationVolume.js';

/* The sound server restores the saved volume once the sink appears */
const SETTLE_US = 2 * GLib.USEC_PER_SEC;

const ICONS = [
    'audio-volume-muted-symbolic',
    'audio-volume-low-symbolic',
    'audio-volume-medium-symbolic',
    'audio-volume-high-symbolic',
    'audio-volume-overamplified-symbolic',
];

/* GNOME 49 replaced show(monitorIndex, …) with show(icon, label, levels) */
export function showOsd(icon, label, level, maxLevel) {
    const manager = Main.osdWindowManager;
    if (manager.showAll)
        manager.showAll(icon, label, level, maxLevel);
    else
        manager.show(-1, icon, label, level, maxLevel);
}

export class VolumeOsd {
    /* getAddress: the AirPods' address; isOwnChange: true while EarPort
     * itself moves the volume (conversation awareness) */
    constructor(getAddress, isOwnChange) {
        this._getAddress = getAddress;
        this._isOwnChange = isOwnChange;
        this._sink = null;
        this._soundSettings = new Gio.Settings({schema_id: 'org.gnome.desktop.sound'});

        this._control = Volume.getMixerControl();
        this._control.connectObject(
            'state-changed', () => this._watchDefaultSink(),
            'default-sink-changed', () => this._watchDefaultSink(),
            this);
        this._watchDefaultSink();
    }

    destroy() {
        this._control.disconnectObject(this);
        this._sink?.disconnectObject(this);
        this._sink = null;
    }

    _watchDefaultSink() {
        const sink = this._control.get_default_sink();
        if (sink === this._sink)
            return;

        this._sink?.disconnectObject(this);
        this._sink = sink;
        this._since = GLib.get_monotonic_time();
        sink?.connectObject(
            'notify::volume', () => this._onVolumeChanged(),
            'notify::is-muted', () => this._onVolumeChanged(),
            this);
    }

    _onVolumeChanged() {
        const sink = this._sink;
        /* The slider in the open menu, or EarPort's own fade, already show
         * the change */
        if (GLib.get_monotonic_time() - this._since < SETTLE_US ||
            Main.panel.statusArea.quickSettings.menu.isOpen ||
            this._isOwnChange() ||
            getAirPodsSink(this._getAddress()) !== sink)
            return;

        /* Same OSD as gnome-settings-daemon's for the volume keys */
        const norm = this._control.get_vol_max_norm();
        const level = sink.is_muted ? 0 : sink.volume / norm;
        const maxLevel = this._soundSettings.get_boolean('allow-volume-above-100-percent')
            ? this._control.get_vol_max_amplified() / norm : 1;
        let n = 0;
        if (level > 1)
            n = ICONS.length - 1;
        else if (level > 0)
            n = Math.clamp(Math.ceil(3 * level), 1, 3);

        showOsd(Gio.ThemedIcon.new(ICONS[n]),
            this._control.lookup_device_from_stream(sink)?.get_description() ?? null,
            level, maxLevel);
    }
}
