/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Battery gauge of the Quick Settings menu
 */

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Cairo from 'cairo';

import {gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

/* Ring geometry (logical px; the widget size comes from CSS) */
const RING_LINE_WIDTH = 4;
const RING_ICON_SIZE = 20;

/* Battery indicator widget: circular progress ring around a symbolic icon,
 * with percentage and name below. The ring color comes from the widget's
 * themed foreground color, so all state styling lives in the stylesheet. */
export const BatteryIndicator = GObject.registerClass(
class BatteryIndicator extends St.BoxLayout {
    constructor(type, label, gicon, chargingGicon) {
        super({
            style_class: 'earport-battery-indicator',
            vertical: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._type = type; // 'left', 'right', 'case'
        this._label = label;
        this._defaultGicon = gicon;

        this._ring = new St.DrawingArea({
            style_class: 'earport-battery-ring',
        });
        this._ring.connect('repaint', area => this._drawRing(area));
        this._ring.connect('style-changed', () => this._ring.queue_repaint());

        this._icon = new St.Icon({
            gicon,
            icon_size: RING_ICON_SIZE,
            style_class: 'earport-battery-icon',
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
            y_expand: true,
        });

        /* Stack the icon on top of the ring */
        this._ringBin = new St.Widget({
            layout_manager: new Clutter.BinLayout(),
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._ringBin.add_child(this._ring);
        this._ringBin.add_child(this._icon);

        /* Charging is shown by a bolt, not only by the ring color */
        this._chargingIcon = new St.Icon({
            gicon: chargingGicon,
            style_class: 'earport-battery-charging-icon',
            y_align: Clutter.ActorAlign.CENTER,
            visible: false,
        });

        this._levelLabel = new St.Label({
            text: '—',
            style_class: 'earport-battery-level',
            y_align: Clutter.ActorAlign.CENTER,
        });

        const levelBox = new St.BoxLayout({
            style_class: 'earport-battery-level-box',
            x_align: Clutter.ActorAlign.CENTER,
        });
        levelBox.add_child(this._chargingIcon);
        levelBox.add_child(this._levelLabel);

        this._nameLabel = new St.Label({
            text: label,
            style_class: 'earport-battery-name',
            opacity: 160,
        });

        this.add_child(this._ringBin);
        this.add_child(levelBox);
        this.add_child(this._nameLabel);

        this._level = -1;
        this._charging = false;
        this._currentStyleState = null;
    }

    setHeadphonesMode(isHeadphones, deviceModel = null) {
        this._isHeadphones = isHeadphones;

        if (this._type === 'left') {
            /* For headphones, left indicator becomes the unified battery */
            if (isHeadphones) {
                this._nameLabel.text = deviceModel || _('Headphones');
                this._icon.gicon = Gio.ThemedIcon.new('audio-headphones-symbolic');
            } else {
                this._nameLabel.text = this._label;
                this._icon.gicon = this._defaultGicon;
            }
            this.visible = true;
        } else if (this._type === 'right' || this._type === 'case') {
            /* Hide right and case for headphones (AirPods Max) */
            this.visible = !isHeadphones;
        }
    }

    setLevel(level, charging = false) {
        /* Skip update if nothing changed */
        if (this._level === level && this._charging === charging)
            return;

        this._level = level;
        this._charging = charging;

        this._chargingIcon.visible = level >= 0 && charging;

        if (level < 0) {
            this._levelLabel.text = '—';
            this._ringBin.opacity = 128;
            this._setStyleState(null);
        } else {
            this._levelLabel.text = `${level}%`;
            this._ringBin.opacity = 255;

            if (charging) {
                this._setStyleState('charging');
            } else if (level <= 10) {
                this._setStyleState('critical');
            } else if (level <= 20) {
                this._setStyleState('low');
            } else {
                this._setStyleState(null);
            }
        }

        this._ring.queue_repaint();
    }

    /* Spoken summary, e.g. "Left 16%, charging" */
    get accessibleText() {
        const name = this._nameLabel.text;
        if (this._level < 0)
            return `${name} ${_('unavailable')}`;
        const text = `${name} ${this._level}%`;
        return this._charging ? `${text}, ${_('charging')}` : text;
    }

    _setStyleState(state) {
        if (this._currentStyleState === state)
            return;

        if (this._currentStyleState)
            this._ring.remove_style_class_name(this._currentStyleState);
        if (state)
            this._ring.add_style_class_name(state);

        this._currentStyleState = state;
    }

    _drawRing(area) {
        const cr = area.get_context();
        const themeNode = area.get_theme_node();
        const [width, height] = area.get_surface_size();

        const cx = width / 2;
        const cy = height / 2;
        const radius = Math.min(width, height) / 2 - RING_LINE_WIDTH / 2;
        const color = themeNode.get_foreground_color();

        cr.setLineWidth(RING_LINE_WIDTH);

        /* Track: themed color, well faded */
        cr.setSourceRGBA(color.red / 255, color.green / 255, color.blue / 255,
            (color.alpha / 255) * 0.25);
        cr.arc(cx, cy, radius, 0, 2 * Math.PI);
        cr.stroke();

        /* Progress arc, clockwise from the top */
        if (this._level >= 0) {
            const fraction = Math.min(this._level, 100) / 100;
            cr.setSourceRGBA(color.red / 255, color.green / 255,
                color.blue / 255, color.alpha / 255);
            cr.setLineCap(Cairo.LineCap.ROUND);
            cr.arc(cx, cy, radius,
                -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * fraction);
            cr.stroke();
        }

        cr.$dispose();
    }
});
