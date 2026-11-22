/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Muting the microphone with the stem during a call, as on an iPhone: the
 * daemon takes single presses from the AirPods while an application
 * records, whatever the input. It mutes the HD microphone itself; for any
 * other input it asks here, and the default input is muted, as with the
 * keyboard's microphone key.
 */

import Gio from 'gi://Gio';
import Gvc from 'gi://Gvc';

import * as Volume from 'resource:///org/gnome/shell/ui/status/volume.js';

import {showOsd} from './volumeOsd.js';

/* As GNOME Shell's microphone indicator: they record to show the level */
const SKIPPED_APPS = ['org.gnome.VolumeControl', 'org.PulseAudio.pavucontrol'];

/* GNOME's microphone OSD; label: the input's name, if known */
export function showMicOsd(muted, label = null) {
    showOsd(Gio.ThemedIcon.new(muted
        ? 'microphone-sensitivity-muted-symbolic'
        : 'microphone-sensitivity-high-symbolic'), label);
}

export class MicMute {
    /* onRecordingChanged(recording): an application started or stopped
     * recording */
    constructor(onRecordingChanged) {
        this._onRecordingChanged = onRecordingChanged;
        this._recording = false;
        this._mutedSource = null;   /* Muted with the stem, unmuted after the call */

        this._control = Volume.getMixerControl();
        this._control.connectObject(
            'state-changed', () => this._update(),
            'stream-added', () => this._update(),
            'stream-removed', () => this._update(),
            'stream-changed', () => this._update(),
            this);
        this._update();
    }

    get recording() {
        return this._recording;
    }

    /* The stem, during a call on another input than the HD microphone */
    toggle() {
        const source = this._control.get_default_source();
        if (!source)
            return;

        const muted = !source.is_muted;
        source.change_is_muted(muted);
        this._mutedSource = muted ? source : null;
        showMicOsd(muted, this._control.lookup_device_from_stream(source)?.get_description() ?? null);
    }

    destroy() {
        this._control.disconnectObject(this);
    }

    _update() {
        const recording = this._control.get_state() === Gvc.MixerControlState.READY &&
            this._control.get_sources().some(source => source.state === Gvc.MixerStreamState.RUNNING) &&
            this._control.get_source_outputs().some(output =>
                !SKIPPED_APPS.includes(output.get_application_id()));
        if (recording === this._recording)
            return;

        this._recording = recording;
        /* The call ended muted: the microphone is back for the next one */
        if (!recording && this._mutedSource) {
            this._mutedSource.change_is_muted(false);
            this._mutedSource = null;
        }
        this._onRecordingChanged(recording);
    }
}
