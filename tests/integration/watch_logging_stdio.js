// Node 22+; run from the repository with the codetopo executable as the argument.
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const assert = require('node:assert/strict');
const { DatabaseSync } = require('node:sqlite');

const exe = path.resolve(process.argv[2]);
const root = path.join(process.cwd(), '.codetopo-watch-logging-test');
let child;
let exited;
let created = false;
let stderr = '';
let protocolError;
let sequence = 0;
const pending = new Map();
const logs = [];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function waitUntil(predicate, description) {
    const deadline = Date.now() + 15000;
    while (!predicate()) {
        if (protocolError) throw protocolError;
        assert(Date.now() < deadline, `${description}\n${stderr}`);
        assert(child.exitCode === null, `Server exited unexpectedly\n${stderr}`);
        await sleep(25);
    }
}

async function request(method, params = {}) {
    const id = ++sequence;
    const response = new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
    child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    let timer;
    try {
        return await Promise.race([response, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${method} timed out\n${stderr}`)), 15000);
        })]);
    } finally {
        clearTimeout(timer);
        pending.delete(id);
    }
}

async function main() {
    assert(!fs.existsSync(root), 'Refusing to overwrite existing fixture');
    fs.mkdirSync(root);
    created = true;
    fs.writeFileSync(path.join(root, 'initial.cpp'), 'int initial() { return 1; }\n');
    const initial = spawnSync(exe, ['index', '--root', root, '--threads', '1', '--arena-size', '8'],
        { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    assert.equal(initial.status, 0, initial.stdout + initial.stderr);

    child = spawn(exe, ['mcp', '--root', root, '--watch', '--freshness=normal'],
        { stdio: ['pipe', 'pipe', 'pipe'] });
    exited = new Promise(resolve => child.once('close', resolve));
    child.on('error', error => { protocolError = error; });
    child.stderr.on('data', data => { stderr += data; });
    readline.createInterface({ input: child.stdout }).on('line', line => {
        try {
            const message = JSON.parse(line);
            if (message.id == null) {
                assert.equal(message.method, 'notifications/message');
                assert.equal(message.params.level, 'info');
                assert.equal(typeof message.params.data, 'string');
                logs.push(message.params.data);
                return;
            }
            const waiter = pending.get(message.id);
            assert(waiter, `Unexpected response: ${line}`);
            waiter.resolve(message);
        } catch (error) {
            protocolError = error;
            for (const waiter of pending.values()) waiter.reject(error);
        }
    });
    assert((await request('initialize')).result.capabilities.logging);
    assert.equal(logs.length, 0);
    child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    await waitUntil(() => logs.some(log => log.includes('live logs enabled')), 'Missing initialization log');
    await waitUntil(() => stderr.includes('reindex: done'), 'Startup supervised index failed');
    assert(!stderr.includes('reindex: failed'), stderr);

    const file = path.join(root, 'changed.cpp');
    for (const operation of ['create', 'modify', 'delete']) {
        const start = logs.length;
        if (operation === 'delete') fs.unlinkSync(file);
        else fs.writeFileSync(file, operation === 'create'
            ? 'int created_symbol() { return 1; }\n'
            : 'int modified_symbol() { return 2; }\n');
        await waitUntil(() => logs.slice(start).some(log => log.includes('watcher: change detected')),
            `Missing ${operation} watcher notification`);
        await waitUntil(() => logs.slice(start).some(log => log.includes('watcher: reindex complete')),
            `${operation} targeted reindex failed`);
        assert(!logs.slice(start).some(log => log.includes('reindex: failed')), stderr);
        const response = await request('tools/call', {
            name: 'symbol_list', arguments: { file_path: 'changed.cpp' }
        });
        assert(!response.error, JSON.stringify(response));
        const result = JSON.parse(response.result.content[0].text);
        if (operation === 'delete') {
            assert.equal(result.total, 0);
            const db = new DatabaseSync(path.join(root, '.codetopo', 'index.sqlite'));
            try {
                assert.equal(db.prepare('SELECT id FROM files WHERE path=?').get('changed.cpp'), undefined);
            } finally { db.close(); }
        } else {
            assert(result.results.some(symbol => symbol.name === `${operation === 'create' ? 'created' : 'modified'}_symbol`),
                JSON.stringify(result));
        }
    }
    console.log('PASS: live protocol logs, startup supervisor, watcher create/modify/delete, index freshness');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    if (child && child.exitCode === null) child.stdin.end();
    if (exited) {
        const code = await exited;
        if (code !== 0) { console.error(stderr); process.exitCode = 1; }
    }
    if (created) fs.rmSync(root, { recursive: true, force: true });
});
