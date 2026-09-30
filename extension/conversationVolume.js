/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Conversation awareness: the AirPods detect speech, lowering the volume
 * is up to the host (as an iPhone does).
 */

import * as Volume from 'resource:///org/gnome/shell/ui/status/volume.js';

export class ConversationVolume {
    constructor(settings) {
        this._settings = settings;
        this._lowered = null;
    }

    /* Lower the volume of the AirPods at this address, if they are the
     * default output */
    lower(address) {
        if (this._lowered)
            return;

        const sink = this._getAirPodsSink(address);
        if (!sink)
            return;

        const saved = sink.volume;
        /* Share of the volume kept while the user speaks */
        const ratio = this._settings.get_int('conversation-volume') / 100;
        const lowered = Math.round(saved * ratio);
        this._lowered = {sink, saved, lowered};
        sink.volume = lowered;
        sink.push_volume();
    }

    restore() {
        if (!this._lowered)
            return;

        const {sink, saved, lowered} = this._lowered;
        this._lowered = null;

        const control = Volume.getMixerControl();
        if (control.lookup_stream_id(sink.id) !== sink)
            return;  /* The AirPods output is gone */

        /* Leave the volume alone if the user changed it meanwhile */
        const tolerance = control.get_vol_max_norm() / 100;
        if (Math.abs(sink.volume - lowered) > tolerance)
            return;

        sink.volume = saved;
        sink.push_volume();
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
