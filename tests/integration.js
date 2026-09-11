#!/usr/bin/env node
"use strict";

// End-to-end coverage for IPC, Lua modes, event enrichment, and hot reload.
const assert = require("node:assert/strict");
const childProcess = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const process = require("node:process");

function delay(milliseconds) {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function waitFor(file, line, child, timeout = 4000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        const contents = fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
        if (contents.split("\n").includes(line)) {
            return;
        }
        if (child.exitCode !== null) {
            throw new Error(`process exited while waiting for ${line}:\n${contents}`);
        }
        await delay(20);
    }
    const contents = fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
    throw new Error(`timed out waiting for ${line} in ${file}:\n${contents}`);
}

async function waitForAdditionalLine(file, line, previousCount, child,
                                     timeout = 4000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        const contents = fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
        const count = contents.split("\n").filter((item) => item === line).length;
        if (count > previousCount) {
            return;
        }
        if (child.exitCode !== null) {
            throw new Error(`process exited while waiting for another ${line}`);
        }
        await delay(20);
    }
    throw new Error(`timed out waiting for another ${line} in ${file}`);
}

function waitForExit(child, timeout = 3000) {
    if (child.exitCode !== null || child.signalCode !== null) {
        return Promise.resolve(child.exitCode ?? child.signalCode);
    }
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("process did not exit")),
                                 timeout);
        child.once("exit", (code, signal) => {
            clearTimeout(timer);
            resolve(code === null ? signal : code);
        });
    });
}

async function stop(child) {
    if (child === null || child.exitCode !== null || child.signalCode !== null) {
        return child === null ? null : (child.exitCode ?? child.signalCode);
    }
    child.kill("SIGTERM");
    try {
        return await waitForExit(child);
    } catch (error) {
        child.kill("SIGKILL");
        await waitForExit(child);
        throw error;
    }
}

async function expectCleanStop(child, stderrPath) {
    const termination = await stop(child);
    assert.equal(termination, 0,
                 `daemon terminated with ${termination}:\n` +
                 fs.readFileSync(stderrPath, "utf8"));
}

function restrictedConfig(version) {
    return `
assert(io == nil and os == nil and package == nil and ffi == nil)
assert(collectgarbage == nil)
local result = exec({"/bin/printf", "exec-ok"})
assert(result.rc == 0 and result.stdout == "exec-ok")
function on_workspace(event)
    print("workspace-${version}:" .. event.change)
end
function on_window(event)
    assert(event.con_id == 42 and event.workspace_num == 2)
    local found = i3.find({con_id = 42, fields = {"name"}, limit = 1})
    assert(#found == 1 and found[1].name == "Example window")
end
`;
}

function runDaemon(binary, socket, configDirectory, stdoutPath, stderrPath,
                   ...extraArguments) {
    const stdout = fs.openSync(stdoutPath, "w");
    const stderr = fs.openSync(stderrPath, "w");
    const child = childProcess.spawn(binary, ["--journald", ...extraArguments], {
        env: {
            ...process.env,
            I3SOCK: socket,
            I3D_DIR: configDirectory,
            I3D_HANDLER_MAX_STEPS: "10000",
            XDG_RUNTIME_DIR: path.dirname(configDirectory),
        },
        stdio: ["ignore", stdout, stderr],
    });
    fs.closeSync(stdout);
    fs.closeSync(stderr);
    return child;
}

async function main() {
    const binary = process.argv[2];
    const fakeServer = process.argv[3];
    const examplesDirectory = process.argv[4];
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "i3d-test-"));
    const socket = path.join(root, "i3.sock");
    const runtimeI3 = path.join(root, "i3");
    const configDirectory = path.join(root, "config");
    const stdoutPath = path.join(root, "stdout.log");
    const stderrPath = path.join(root, "stderr.log");
    fs.mkdirSync(configDirectory);
    fs.mkdirSync(runtimeI3);

    let server = childProcess.spawn(process.execPath, [fakeServer, socket], {
        stdio: "inherit",
    });
    let daemon = null;
    try {
        const deadline = Date.now() + 3000;
        while (!fs.existsSync(socket) && Date.now() < deadline) {
            await delay(20);
        }
        assert(fs.existsSync(socket), "fake i3 socket was not created");

        const config = path.join(configDirectory, "10-test.lua");
        fs.writeFileSync(config, restrictedConfig("v1"));
        const alphaConfig = path.join(configDirectory, "alpha.lua");
        const zetaConfig = path.join(configDirectory, "zeta.lua");
        daemon = runDaemon(binary, socket, configDirectory, stdoutPath,
                           stderrPath);
        await waitFor(stdoutPath, "<6>workspace-v1:focus", daemon);
        await waitFor(stderrPath, "<6>i3d scripts active=1 handlers=2", daemon);

        // Arbitrarily named files are discovered live and dispatched by
        // alphabetical basename, without parsing a numeric priority prefix.
        fs.writeFileSync(alphaConfig,
                         'function on_workspace() print("order-alpha") end\n');
        fs.writeFileSync(zetaConfig,
                         'function on_workspace() print("order-zeta") end\n');
        await waitFor(stdoutPath, "<6>order-alpha", daemon);
        await waitFor(stdoutPath, "<6>order-zeta", daemon);
        await waitFor(stderrPath, "<6>i3d scripts active=3 handlers=4", daemon);
        const orderedOutput = fs.readFileSync(stdoutPath, "utf8");
        assert(orderedOutput.indexOf("<6>order-alpha") <
               orderedOutput.indexOf("<6>order-zeta"));

        // A runaway handler is interrupted while the daemon remains live.
        const limitConfig = path.join(configDirectory, "any-name.lua");
        fs.writeFileSync(limitConfig,
                         "function on_mode() while true do end end\n");
        await waitFor(stderrPath,
                      "<3>i3d handler any-name.lua (mode): " +
                      "instruction limit exceeded in any-name.lua", daemon);
        const activeLine = "<6>i3d scripts active=3 handlers=4";
        const activeCount = fs.readFileSync(stderrPath, "utf8").split("\n")
            .filter((line) => line === activeLine).length;
        fs.unlinkSync(limitConfig);
        await waitForAdditionalLine(stderrPath, activeLine, activeCount, daemon);

        // renameSync models the atomic replacement used by most editors.
        const replacement = `${config}.new`;
        fs.writeFileSync(replacement, restrictedConfig("v2"));
        fs.renameSync(replacement, config);
        await waitFor(stdoutPath, "<6>workspace-v2:focus", daemon);

        // Restart the fake compositor under a different conventional socket
        // name. i3d must discard the stale I3SOCK path and rediscover it.
        await stop(server);
        server = null;
        if (fs.existsSync(socket)) {
            fs.unlinkSync(socket);
        }
        // Bind at the temp root and expose it through the conventional i3
        // discovery directory. Some CI sandboxes disallow socket inodes below
        // a second-level temporary directory but permit symlink traversal.
        const replacementSocket = path.join(root, "i3-restarted.sock");
        server = childProcess.spawn(process.execPath,
                                    [fakeServer, replacementSocket], {
                                        stdio: "inherit",
                                    });
        const socketDeadline = Date.now() + 3000;
        while (!fs.existsSync(replacementSocket) &&
               Date.now() < socketDeadline) {
            await delay(20);
        }
        assert(fs.existsSync(replacementSocket),
               "replacement fake i3 socket was not created");
        fs.mkdirSync(runtimeI3, {recursive: true});
        fs.symlinkSync(replacementSocket,
                       path.join(runtimeI3, "ipc-socket.restarted"));
        fs.writeFileSync(replacement, restrictedConfig("v3"));
        fs.renameSync(replacement, config);
        await waitFor(stdoutPath, "<6>workspace-v3:focus", daemon, 5000);

        fs.unlinkSync(config);
        fs.unlinkSync(alphaConfig);
        fs.unlinkSync(zetaConfig);
        await waitFor(stderrPath, "<6>i3d scripts active=0 handlers=0", daemon);
        await expectCleanStop(daemon, stderrPath);
        daemon = null;

        // Full mode retains package, debug, and FFI alongside the i3d flag.
        fs.writeFileSync(path.join(configDirectory, "20-full.lua"), `
assert(type(package) == "table" and type(debug) == "table")
assert(type(require("ffi")) == "table")
assert(debug.enabled == false and i3d_debug == false)
log("full-lua-ok")
`);
        daemon = runDaemon(binary, socket, configDirectory, stdoutPath,
                           stderrPath, "--full-lua");
        await waitFor(stderrPath, "<6>i3d full-lua-ok", daemon);
        await expectCleanStop(daemon, stderrPath);
        daemon = null;

        // Load the repository examples as a set so syntax and API drift are
        // caught even for examples that a particular event does not exercise.
        fs.unlinkSync(path.join(configDirectory, "20-full.lua"));
        for (const name of fs.readdirSync(examplesDirectory)) {
            if (name.endsWith(".lua")) {
                fs.copyFileSync(path.join(examplesDirectory, name),
                                path.join(configDirectory, name));
            }
        }
        daemon = runDaemon(binary, socket, configDirectory, stdoutPath,
                           stderrPath);
        await waitFor(stderrPath, "<6>i3d scripts active=4 handlers=4", daemon);
        await expectCleanStop(daemon, stderrPath);
        daemon = null;
    } finally {
        await stop(daemon);
        await stop(server);
        fs.rmSync(root, {recursive: true, force: true});
    }
}

main().catch((error) => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
