// Windows deployment smoke test; all fixtures stay beside the tested executable.
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');

assert.equal(process.platform, 'win32', 'This test exercises the Windows DLL loader');
const configMode = process.argv[2] === '--config';
function readConfig(file) {
    const config = JSON.parse(fs.readFileSync(file, 'utf8'));
    const server = (config.mcpServers || config.servers)?.codetopo;
    assert(server?.command && Array.isArray(server.args), `No codetopo server in ${file}`);
    return server;
}
const configured = configMode ? readConfig(process.argv[3]) : null;
const sustained = process.argv.includes('--sustain');
const ambient = process.argv.includes('--ambient');
const stringIds = process.argv.includes('--string-ids');
const skipPing = process.argv.includes('--skip-ping');
const option = (name, fallback) => process.argv.find(arg => arg.startsWith(`${name}=`))
    ?.slice(name.length + 1) ?? fallback;
const durationMs = Number(option('--duration', '30')) * 1000;
const protocolVersion = option('--protocol', '2025-06-18');
const exe = path.resolve(configured ? configured.command : process.argv[2]);
const root = path.join(path.dirname(exe), `portable-startup-${process.pid}`);
const env = Object.fromEntries(Object.entries(process.env)
    .filter(([key]) => key.toLowerCase() !== 'path'));
const systemRoot = Object.entries(env).find(([key]) => key.toLowerCase() === 'systemroot')?.[1];
assert(systemRoot, 'SystemRoot must be set');
env.PATH = ambient ? process.env.PATH : `${path.join(systemRoot, 'System32')};${systemRoot}`;
let child;
let closed;
let stderr = '';
let created = false;
let timer;

async function main() {
    if (configMode) {
        await handshake(exe, configured.args, path.resolve(process.argv[4] || '.'),
            configured.env || {});
        return;
    }
    assert(!fs.existsSync(root), 'Refusing to overwrite existing fixture');
    fs.mkdirSync(root);
    created = true;
    const version = spawnSync(exe, ['--version'],
        { env, cwd: root, encoding: 'utf8', timeout: 10000 });
    assert.ifError(version.error);
    assert.equal(version.status, 0, version.stderr);
    assert.match(version.stdout.trim(), /^\d+\.\d+\.\d+$/);

    fs.writeFileSync(path.join(root, 'example.cpp'), 'int portable_example() { return 1; }\n');
    for (const [file, key] of [
        ['.mcp.json', 'mcpServers'], ['.github\\mcp.json', 'mcpServers'],
        ['.vscode\\mcp.json', 'servers'],
    ]) {
        const configFile = path.join(root, file);
        fs.mkdirSync(path.dirname(configFile), { recursive: true });
        fs.writeFileSync(configFile, JSON.stringify({
            [key]: { other: { command: 'other-server', args: [] } },
        }));
    }
    const indexed = spawnSync(exe,
        ['init', '--root', root, '--threads', '1', '--arena-size', '8',
            '--editors', 'vscode,copilot', '--freshness', 'off'],
        { env, cwd: root, encoding: 'utf8', timeout: 30000 });
    assert.ifError(indexed.error);
    assert.equal(indexed.status, 0, indexed.stdout + indexed.stderr);
    assert(fs.existsSync(path.join(root, '.codetopo', 'index.sqlite')));

    for (const file of ['.mcp.json', '.github\\mcp.json', '.vscode\\mcp.json']) {
        const configFile = path.join(root, file);
        const config = JSON.parse(fs.readFileSync(configFile, 'utf8'));
        assert.equal((config.mcpServers || config.servers).other.command, 'other-server');
        const server = readConfig(configFile);
        assert.equal(server.command.toLowerCase(), exe.toLowerCase());
        assert.equal(server.args[server.args.indexOf('--root') + 1], root);
        await handshake(server.command, server.args, path.dirname(exe));
    }
    console.log(`PASS: ${version.stdout.trim()}, supervised init and generated configs with unrelated cwd`);
}

async function handshake(command, args, cwd, serverEnv = {}) {
    if (sustained) return sustainedSession(command, args, cwd, serverEnv);
    const started = Date.now();
    stderr = '';
    child = spawn(command, args,
        { env: { ...env, ...serverEnv }, cwd, stdio: ['pipe', 'pipe', 'pipe'] });
    closed = new Promise(resolve => child.once('close', resolve));
    child.stderr.on('data', data => { stderr += data; });
    let toolCount;
    let handshakeMs;
    await new Promise((resolve, reject) => {
        timer = setTimeout(() => reject(new Error(`MCP handshake timed out\n${stderr}`)), 15000);
        child.once('error', reject);
        child.once('exit', code => reject(new Error(`MCP exited early: ${code}\n${stderr}`)));
        readline.createInterface({ input: child.stdout }).on('line', line => {
            try {
                const message = JSON.parse(line);
                assert.equal(message.jsonrpc, '2.0');
                assert(!message.error, line);
                if (message.id === 1) {
                    assert(message.result.capabilities.tools);
                    child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
                    child.stdin.write('{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}\n');
                } else if (message.id === 2) {
                    assert(message.result.tools.length > 0);
                    toolCount = message.result.tools.length;
                    handshakeMs = Date.now() - started;
                    clearTimeout(timer);
                    resolve();
                } else {
                    assert.equal(message.method, 'notifications/message', line);
                }
            } catch (error) { reject(error); }
        });
        child.stdin.write(JSON.stringify({
            jsonrpc: '2.0', id: 1, method: 'initialize',
            params: { protocolVersion: '2024-11-05', capabilities: {},
                clientInfo: { name: 'portable-startup', version: '1' } },
        }) + '\n');
    });
    child.stdin.end();
    const exit = await Promise.race([closed, new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`MCP shutdown timed out\n${stderr}`)), 30000);
    })]);
    clearTimeout(timer);
    assert.equal(exit, 0, stderr);
    console.log(`PASS: initialize + tools/list (${toolCount} tools), exit=${exit}, ` +
        `handshake=${handshakeMs}ms, elapsed=${Date.now() - started}ms, system-only PATH, cwd=${cwd}`);
    if (configMode && stderr) console.log(`stderr:\n${stderr.trim()}`);
}

// Validate every advertised schema, not only the tool count. This is structural
// validation of the JSON Schema vocabulary used here, not a host SDK validator.
function validateSchema(schema, location) {
    assert(schema && typeof schema === 'object' && !Array.isArray(schema), location);
    if (schema.type !== undefined) {
        const types = Array.isArray(schema.type) ? schema.type : [schema.type];
        assert(types.every(type => ['object', 'array', 'string', 'number', 'integer',
            'boolean', 'null'].includes(type)), `${location}.type`);
    }
    if (schema.properties) {
        assert.equal(typeof schema.properties, 'object', `${location}.properties`);
        for (const [key, value] of Object.entries(schema.properties))
            validateSchema(value, `${location}.${key}`);
    }
    if (schema.required) {
        assert(Array.isArray(schema.required), `${location}.required`);
        assert(schema.required.every(key => typeof key === 'string'), `${location}.required`);
    }
    for (const key of ['anyOf', 'oneOf', 'allOf']) {
        if (schema[key]) {
            assert(Array.isArray(schema[key]) && schema[key].length, `${location}.${key}`);
            schema[key].forEach((value, index) => validateSchema(value, `${location}.${key}[${index}]`));
        }
    }
    if (schema.items) validateSchema(schema.items, `${location}.items`);
}

async function sustainedSession(command, args, cwd, serverEnv) {
    assert(Number.isFinite(durationMs) && durationMs >= 1000, 'Invalid sustained duration');
    const started = Date.now();
    const pending = new Map();
    let sequence = 0, notifications = 0, stdoutBytes = 0, lastCompleted = 'none';
    let protocolError;
    stderr = '';
    const trace = message => console.log(`[${Date.now() - started}ms] ${message}`);
    child = spawn(command, args,
        { env: { ...env, ...serverEnv }, cwd, stdio: ['pipe', 'pipe', 'pipe'] });
    closed = new Promise(resolve => child.once('close', resolve));
    const fail = error => {
        protocolError = error;
        for (const waiter of pending.values()) waiter.reject(error);
    };
    child.once('error', fail);
    child.once('exit', (code, signal) => {
        trace(`exit code=${code} signal=${signal}`);
        if (pending.size) fail(new Error(`Server exited with requests pending: ${code}`));
    });
    child.stderr.on('data', data => {
        stderr += data;
        stderr = stderr.slice(-20000);
        for (const line of data.toString().trim().split('\n')) trace(`stderr ${line.trim()}`);
    });
    child.stdout.on('data', data => { stdoutBytes += data.length; });
    const lines = readline.createInterface({ input: child.stdout });
    lines.on('line', line => {
        try {
            const message = JSON.parse(line);
            assert.equal(message.jsonrpc, '2.0', line);
            if (!Object.hasOwn(message, 'id')) {
                assert.equal(message.method, 'notifications/message', line);
                assert.equal(message.params.level, 'info', line);
                assert.equal(typeof message.params.data, 'string', line);
                notifications++;
                return;
            }
            const waiter = pending.get(message.id);
            if (!waiter) {
                trace(`UNMATCHED response id=${JSON.stringify(message.id)}; pending=${JSON.stringify([...pending.keys()])}`);
                return;
            }
            assert(Object.hasOwn(message, 'result') !== Object.hasOwn(message, 'error'), line);
            pending.delete(message.id);
            lastCompleted = `${waiter.method} id=${JSON.stringify(message.id)}`;
            trace(`recv ${lastCompleted} duration=${Date.now() - waiter.sent}ms bytes=${Buffer.byteLength(line)}`);
            waiter.resolve(message);
        } catch (error) { fail(error); }
    });
    async function request(method, params = {}, allowError = false) {
        if (protocolError) throw protocolError;
        const id = stringIds ? `startup-${++sequence}` : ++sequence;
        let timeout;
        const response = new Promise((resolve, reject) => {
            pending.set(id, { resolve, reject, method, sent: Date.now() });
            timeout = setTimeout(() => reject(new Error(
                `HANG: ${method} id=${JSON.stringify(id)}, lastCompleted=${lastCompleted}, ` +
                `pending=${JSON.stringify([...pending.keys()])}, alive=${child.exitCode === null}\n${stderr}`)), 5000);
        });
        trace(`send ${method} id=${JSON.stringify(id)}`);
        child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
        try {
            const message = await response;
            if (!allowError) assert(!message.error, JSON.stringify(message));
            return message;
        } finally { clearTimeout(timeout); pending.delete(id); }
    }
    const tool = async (name, arguments = {}) => {
        const response = await request('tools/call', { name, arguments });
        assert(Array.isArray(response.result.content), JSON.stringify(response));
        assert.equal(response.result.isError ?? false, false, JSON.stringify(response));
        const content = response.result.content.find(item => item.type === 'text');
        assert.equal(typeof content?.text, 'string', JSON.stringify(response));
        return JSON.parse(content.text);
    };
    const discovery = async () => {
        const response = await request('tools/list');
        const tools = response.result.tools;
        assert(Array.isArray(tools) && tools.length > 0);
        assert.equal(new Set(tools.map(tool => tool.name)).size, tools.length);
        for (const tool of tools) {
            assert.match(tool.name, /^[a-zA-Z0-9_-]{1,64}$/);
            assert.equal(typeof tool.description, 'string', tool.name);
            assert.equal(tool.inputSchema?.type, 'object', tool.name);
            validateSchema(tool.inputSchema, tool.name);
        }
        trace(`validated all ${tools.length} tool schemas and metadata`);
        return tools;
    };
    try {
        trace(`spawn pid=${child.pid}, PATH=${ambient ? 'ambient' : 'system-only'}, protocol=${protocolVersion}`);
        const init = (await request('initialize', {
            protocolVersion, capabilities: { roots: { listChanged: true } },
            clientInfo: { name: 'client-like-startup-probe', version: '1' },
        })).result;
        assert(['2024-11-05', '2025-03-26', '2025-06-18', '2025-11-25'].includes(init.protocolVersion));
        assert.equal(typeof init.serverInfo.name, 'string');
        assert.equal(typeof init.serverInfo.version, 'string');
        assert.equal(typeof init.instructions, 'string');
        assert(init.capabilities.tools && init.capabilities.logging);
        trace(`negotiated protocol=${init.protocolVersion}`);
        child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
        // These requests are deliberately pipelined during background reindex.
        await Promise.all([discovery(), tool('server_health'), tool('server_info'),
            tool('workspace_list'), tool('symbol_search', { query: 'McpServer', limit: 3 })]);
        if (!skipPing) assert.deepEqual((await request('ping')).result, {});
        assert.deepEqual((await request('logging/setLevel', { level: 'warning' })).result, {});
        const filtered = notifications;
        await tool('server_info');
        assert.equal(notifications, filtered, 'Logging filter did not suppress info notifications');
        assert.equal((await request('logging/setLevel', { level: 'invalid' }, true)).error.code, -32602);
        assert.equal((await request('not/a/method', {}, true)).error.code, -32601);
        assert.deepEqual((await request('logging/setLevel', { level: 'info' })).result, {});
        child.stdin.write('{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"not-pending","reason":"probe"}}\n');
        while (Date.now() - started < durationMs) {
            await new Promise(resolve => setTimeout(resolve, 1000));
            assert.equal(child.exitCode, null, 'Server exited while stdin remained open');
            await Promise.all([discovery(), tool('server_health'), tool('server_info')]);
            if (!skipPing) assert.deepEqual((await request('ping')).result, {});
        }
        const backgroundReindex = stderr.includes('reindex: failed') ? 'failed'
            : stderr.includes('reindex: done') ? 'done'
            : stderr.includes('reindex: started') ? 'running' : 'not-started';
        trace(`sustained protocol PASS backgroundReindex=${backgroundReindex} ` +
            `notifications=${notifications} stdoutBytes=${stdoutBytes} lastCompleted=${lastCompleted}`);
    } finally {
        child.stdin.end();
        let shutdownTimer;
        try {
            const exit = await Promise.race([closed, new Promise((_, reject) => {
                shutdownTimer = setTimeout(() => reject(new Error('Shutdown exceeded 15s')), 15000);
            })]);
            assert.equal(exit, 0, stderr);
        } finally { clearTimeout(shutdownTimer); lines.close(); }
    }
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    clearTimeout(timer);
    if (child && child.exitCode === null && child.signalCode === null) child.kill();
    if (closed) await closed;
    if (created) fs.rmSync(root, { recursive: true, force: true });
});
