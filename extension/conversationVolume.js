/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Conversation awareness: the AirPods detect speech, lowering the volume
 * is up to the host (as an iPhone does). The volume fades: down quickly so
 * the other person is heard at once, back up slowly.
 */

import GLib from 'gi://GLib';

import * as Volume from 'resource:///org/gnome/shell/ui/status/volume.js';

const LOWER_MS = 400;
const RESTORE_MS = 1500;
const STEP_MS = 20;

/* Slow at both ends: smoother to the ear than a straight line */
function ease(t) {
    return t * t * (3 - 2 * t);
}

export class ConversationVolume {
    constructor(settings) {
        this._settings = settings;
        /* {sink, saved, lowered} while the volume is lowered or restoring */
        this._lowered = null;
        this._fadeId = 0;
        this._expected = null;  /* Last volume we set */
    }

    /* Lower the volume of the AirPods at this address, if they are the
     * default output */
    lower(address) {
        if (this._lowered) {
            /* Speaking again while it was coming back up: down again from
             * where it is */
            if (this._restoring)
                this._fadeTo(this._lowered.lowered, LOWER_MS, false);
            return;
        }

        const sink = this._getAirPodsSink(address);
        if (!sink)
            return;

        const saved = sink.volume;
        /* Share of the volume kept while the user speaks */
        const ratio = this._settings.get_int('conversation-volume') / 100;
        const lowered = Math.round(saved * ratio);
        this._lowered = {sink, saved, lowered};
        this._expected = saved;
        this._fadeTo(lowered, LOWER_MS, false);
    }

    restore() {
        if (!this._lowered || this._restoring || !this._sinkUntouched())
            return;

        this._fadeTo(this._lowered.saved, RESTORE_MS, true);
    }

    /* Extension disabled: back to the saved volume at once */
    destroy() {
        this._stopFade();
        if (this._lowered && this._sinkUntouched()) {
            this._lowered.sink.volume = this._lowered.saved;
            this._lowered.sink.push_volume();
        }
        this._lowered = null;
    }

    _fadeTo(target, duration, restoring) {
        this._stopFade();

        const {sink} = this._lowered;
        const start = sink.volume;
        const steps = Math.max(1, Math.round(duration / STEP_MS));
        let step = 0;
        this._restoring = restoring;

        this._fadeId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, STEP_MS, () => {
            if (!this._sinkUntouched()) {
                this._fadeId = 0;
                return GLib.SOURCE_REMOVE;
            }

            step++;
            this._expected = Math.round(start + (target - start) * ease(step / steps));
            sink.volume = this._expected;
            sink.push_volume();

            if (step < steps)
                return GLib.SOURCE_CONTINUE;

            this._fadeId = 0;
            if (restoring)
                this._lowered = null;
            return GLib.SOURCE_REMOVE;
        });
    }

    _stopFade() {
        if (this._fadeId) {
            GLib.source_remove(this._fadeId);
            this._fadeId = 0;
        }
        this._restoring = false;
    }

    /* The AirPods output is still there and the user didn't change its
     * volume meanwhile; otherwise forget about restoring it */
    _sinkUntouched() {
        const {sink} = this._lowered;
        const control = Volume.getMixerControl();
        const tolerance = control.get_vol_max_norm() / 100;

        if (control.lookup_stream_id(sink.id) !== sink ||
            Math.abs(sink.volume - this._expected) > tolerance) {
            this._stopFade();
            this._lowered = null;
            return false;
        }
        return true;
    }

    /* Default output, only if it is these AirPods */
    _getAirPodsSink(address) {
        const sink = Volume.getMixerControl().get_default_sink();
        if (!address || !sink)
            return null;

        /* bluez_output.AC_07_75_F0_31_02.1 (PipeWire), bluez_sink.… (PulseAudio) */
        const id = address.replace(/:/g, '_').toUpperCase();
        return sink.get_name()?.toUpperCase().includes(id) ? sink : null;
    }
}
