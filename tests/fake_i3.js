#!/usr/bin/env node
"use strict";

// A wire-level i3 server keeps integration tests independent of X and i3.
const net = require("node:net");
const process = require("node:process");

const magic = Buffer.from("i3-ipc");
const headerLength = 14;
const eventBit = 0x80000000;

function message(type, value) {
    const payload = Buffer.from(JSON.stringify(value));
    const header = Buffer.alloc(headerLength);
    magic.copy(header);
    header.writeUInt32LE(payload.length, 6);
    header.writeUInt32LE(type >>> 0, 10);
    return Buffer.concat([header, payload]);
}

function syntheticTree() {
    // This is the smallest tree that exercises workspace and window helpers.
    const window = {
        id: 42,
        type: "con",
        name: "Example window",
        pid: process.pid,
        focused: true,
        fullscreen_mode: 2,
        window_properties: {instance: "firefox", class: "Firefox"},
        nodes: [],
        floating_nodes: [],
    };
    const workspace = {
        id: 20,
        type: "workspace",
        num: 2,
        name: "2:web",
        nodes: [window],
        floating_nodes: [],
    };
    return {id: 1, type: "root", nodes: [workspace], floating_nodes: []};
}

function responseFor(type, payload) {
    switch (type) {
    case 0:
        return [{success: true}];
    case 1:
        return [{num: 2, name: "2:web", focused: true}];
    case 3:
        return [];
    case 4:
        return syntheticTree();
    case 5:
        return [];
    case 6:
        return payload.length === 0 ? ["bar-0"] : {id: payload.toString()};
    case 7:
        return {major: 4, minor: 24, human_readable: "fake i3"};
    default:
        return null;
    }
}

const server = net.createServer((socket) => {
    let input = Buffer.alloc(0);
    let eventTimer = null;

    socket.on("data", (chunk) => {
        input = Buffer.concat([input, chunk]);
        while (input.length >= headerLength) {
            if (!input.subarray(0, 6).equals(magic)) {
                socket.destroy();
                return;
            }
            const length = input.readUInt32LE(6);
            const type = input.readUInt32LE(10);
            if (input.length < headerLength + length) {
                return;
            }
            const payload = input.subarray(headerLength, headerLength + length);
            input = input.subarray(headerLength + length);
            if (type === 2) {
                socket.write(message(type, {success: true}));
                // A short event cycle makes reload and execution limits
                // observable without slowing the integration test.
                let eventIndex = 0;
                eventTimer = setInterval(() => {
                    if (eventIndex === 0) {
                        socket.write(message(eventBit, {change: "focus"}));
                    } else if (eventIndex === 1) {
                        socket.write(message(eventBit | 3, {
                            change: "fullscreen_mode",
                            container: {id: 42, fullscreen_mode: 2},
                        }));
                    } else {
                        socket.write(message(eventBit | 2, {change: "default"}));
                    }
                    eventIndex = (eventIndex + 1) % 3;
                }, 80);
            } else {
                socket.write(message(type, responseFor(type, payload)));
            }
        }
    });
    socket.on("close", () => {
        if (eventTimer !== null) {
            clearInterval(eventTimer);
        }
    });
    // A daemon disconnect during teardown is an expected socket condition.
    socket.on("error", () => {});
});

server.listen(process.argv[2]);
