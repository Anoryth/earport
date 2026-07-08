/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 *
 * Shared D-Bus interface definition for org.librepods.AirPods1.
 * Single source of truth used by both extension.js and prefs.js —
 * keep in sync with daemon/src/dbus_service.c.
 */

export const BUS_NAME = 'org.librepods.Daemon';
export const OBJECT_PATH = '/org/librepods/AirPods';

export const AirPodsInterface = `
<node>
  <interface name="org.librepods.AirPods1">
    <property name="Connected" type="b" access="read"/>
    <property name="DeviceName" type="s" access="read"/>
    <property name="DeviceAddress" type="s" access="read"/>
    <property name="DeviceModel" type="s" access="read"/>
    <property name="DisplayName" type="s" access="read"/>
    <property name="IsHeadphones" type="b" access="read"/>
    <property name="SupportsANC" type="b" access="read"/>
    <property name="SupportsAdaptive" type="b" access="read"/>
    <property name="BatteryLeft" type="i" access="read"/>
    <property name="BatteryRight" type="i" access="read"/>
    <property name="BatteryCase" type="i" access="read"/>
    <property name="ChargingLeft" type="b" access="read"/>
    <property name="ChargingRight" type="b" access="read"/>
    <property name="ChargingCase" type="b" access="read"/>
    <property name="NoiseControlMode" type="s" access="read"/>
    <property name="ConversationalAwareness" type="b" access="read"/>
    <property name="LeftInEar" type="b" access="read"/>
    <property name="RightInEar" type="b" access="read"/>
    <property name="AdaptiveNoiseLevel" type="i" access="read"/>
    <property name="EarPauseMode" type="i" access="read"/>
    <property name="ListeningModeOff" type="b" access="read"/>
    <property name="ListeningModeTransparency" type="b" access="read"/>
    <property name="ListeningModeANC" type="b" access="read"/>
    <property name="ListeningModeAdaptive" type="b" access="read"/>
    <method name="SetNoiseControlMode">
      <arg type="s" name="mode" direction="in"/>
    </method>
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
    <signal name="DeviceConnected">
      <arg type="s" name="address"/>
      <arg type="s" name="name"/>
    </signal>
    <signal name="DeviceDisconnected">
      <arg type="s" name="address"/>
      <arg type="s" name="name"/>
    </signal>
    <signal name="BatteryChanged">
      <arg type="i" name="left"/>
      <arg type="i" name="right"/>
      <arg type="i" name="case_battery"/>
    </signal>
    <signal name="NoiseControlModeChanged">
      <arg type="s" name="mode"/>
    </signal>
    <signal name="EarDetectionChanged">
      <arg type="b" name="leftInEar"/>
      <arg type="b" name="rightInEar"/>
    </signal>
  </interface>
</node>
`;
