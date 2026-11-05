// Zigbee2MQTT external converter for the greenlight e-paper display (docs/zigbee.md#external-converter; rev 7 data model).
// Install as data/external_converters/greenlight.mjs (advanced.enable_external_js must be on), then restart Z2M.
// Written against Z2M 2.12 / zigbee-herdsman-converters 26.76 / zigbee-herdsman 10.6.
//
// No configure step: the device reports everything on its own (freeze rule 3), and reads to the sleepy device
// would fail, so every expose is STATE only. HA writes the display input with Z2M's raw `write` (docs/zigbee.md#input-ha-to-device);
// the input attributes are declared too, so they can also be written by name.

import {Zcl} from "zigbee-herdsman";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

const T = Zcl.DataType;

export default {
    fingerprint: [{modelID: "greenlight-eink42", manufacturerName: "Greenlight"}],
    model: "greenlight-eink42",
    vendor: "Greenlight",
    description: "E-paper problem list display",
    extend: [
        m.deviceAddCustomCluster("greenlight", {
            name: "greenlight",
            ID: 0xfc8a,
            attributes: {
                // input, written by HA (0x0000-0x0007 retired, 0x0010-0x0014 reserved)
                viewBits: {name: "viewBits", ID: 0x0020, type: T.UINT32, write: true, max: 0xffffffff},
                viewDs: {name: "viewDs", ID: 0x0021, type: T.UINT32, write: true, max: 0xffffffff},
                viewTime: {name: "viewTime", ID: 0x0022, type: T.UINT32, write: true, max: 0xffffffff},
                viewId: {name: "viewId", ID: 0x0023, type: T.UINT32, write: true, max: 0xffffffff},
                dsEntry: {name: "dsEntry", ID: 0x0030, type: T.CHAR_STR, write: true},
                dsCommit: {name: "dsCommit", ID: 0x0031, type: T.UINT32, write: true, max: 0xffffffff},
                // state, reported by the device
                batteryMv: {name: "batteryMv", ID: 0x0013, type: T.UINT16},
                wakeCount: {name: "wakeCount", ID: 0x0040, type: T.UINT32},
                shownViewId: {name: "shownViewId", ID: 0x0041, type: T.UINT32},
                datasetHash: {name: "datasetHash", ID: 0x0042, type: T.UINT32},
                awake: {name: "awake", ID: 0x0043, type: T.BOOLEAN},
            },
            commands: {},
            commandsResponse: {},
        }),
        m.battery({percentage: true, voltage: false, lowStatus: true, percentageReporting: false}),
        m.occupancy({reporting: false}),
        m.numeric({
            name: "voltage",
            cluster: "greenlight",
            attribute: "batteryMv",
            description: "Battery voltage (exact; the standard attribute has 100 mV steps)",
            unit: "mV",
            access: "STATE",
            entityCategory: "diagnostic",
        }),
        m.numeric({
            name: "wake_count",
            cluster: "greenlight",
            attribute: "wakeCount",
            description: "Wake counter, +1 on every wake; HA's push trigger",
            access: "STATE",
            entityCategory: "diagnostic",
        }),
        m.numeric({
            name: "shown_view_id",
            cluster: "greenlight",
            attribute: "shownViewId",
            description: "Id of the view on the panel (0 = none)",
            access: "STATE",
            entityCategory: "diagnostic",
        }),
        m.numeric({
            name: "dataset_hash",
            cluster: "greenlight",
            attribute: "datasetHash",
            description: "Hash of the stored dataset (0 = none)",
            access: "STATE",
            entityCategory: "diagnostic",
        }),
        m.binary({
            name: "awake",
            cluster: "greenlight",
            attribute: "awake",
            valueOn: [true, 1],
            valueOff: [false, 0],
            description: "Joined and listening: pushes are delivered now",
            access: "STATE",
            entityCategory: "diagnostic",
        }),
    ],
};
