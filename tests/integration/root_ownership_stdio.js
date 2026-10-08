// Node 22+ (built-in SQLite). Exercises production MCP startup from a non-primary CWD.
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const { DatabaseSync } = require('node:sqlite');

const exe = path.resolve(process.argv[2]);
const base = path.join(process.cwd(), '.codetopo-root-ownership-stdio-test');
const primary = path.join(base, 'primary');
const extra = path.join(base, 'extra');
const dbPath = path.join(primary, '.codetopo', 'index.sqlite');
const clients = [];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let created = false;

function run(args, cwd = base) {
    const result = spawnSync(exe, args, { cwd, encoding: 'utf8', timeout: 120000 });
    assert.ifError(result.error);
    assert.equal(result.status, 0, `${args.join(' ')}\n${result.stdout}\n${result.stderr}`);
    return result;
}

class Client {
    constructor() {
        this.child = spawn(exe, [
            'mcp', '--root', primary, '--db', dbPath,
            '--freshness', 'normal', '--watch', '--debounce', '50',
        ], { cwd: extra, stdio: ['pipe', 'pipe', 'pipe'] });
        this.sequence = 0;
        this.pending = new Map();
        this.logs = '';
        this.protocolError = null;
        this.closed = new Promise(resolve => this.child.once('close', resolve));
        this.child.stderr.on('data', data => { this.logs += data.toString(); });
        this.child.stdin.on('error', () => {});
        this.lines = readline.createInterface({ input: this.child.stdout });
        this.lines.on('line', line => {
            try {
                const message = JSON.parse(line);
                if (!Object.hasOwn(message, 'id')) return;
                const pending = this.pending.get(message.id);
                assert(pending, `Unexpected response: ${line}`);
                this.pending.delete(message.id);
                pending.resolve(message);
            } catch (error) {
                this.protocolError = error;
                for (const pending of this.pending.values()) pending.reject(error);
                this.pending.clear();
            }
        });
        clients.push(this);
    }

    async request(method, params = {}, timeoutMs = 10000) {
        if (this.protocolError) throw this.protocolError;
        const id = ++this.sequence;
        let timer;
        const response = new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            timer = setTimeout(() => reject(new Error(
                `Timed out waiting for ${method}\n${this.logs}`)), timeoutMs);
        });
        this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
        try {
            const message = await response;
            assert(!message.error, JSON.stringify(message));
            return message.result;
        } finally {
            clearTimeout(timer);
            this.pending.delete(id);
        }
    }

    async initialize() {
        await this.request('initialize', {
            protocolVersion: '2024-11-05',
            capabilities: {},
            clientInfo: { name: 'root-ownership-regression', version: '1' },
        });
        this.child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    }

    async tool(name, args = {}) {
        const result = await this.request('tools/call', { name, arguments: args });
        const text = result.content.find(item => item.type === 'text');
        assert(text, JSON.stringify(result));
        return JSON.parse(text.text);
    }

    async close() {
        if (this.child.exitCode === null) this.child.stdin.end();
        const code = await this.closed;
        this.lines.close();
        assert.equal(code, 0, this.logs);
        return this.logs;
    }
}

function openDb() {
    return new DatabaseSync(dbPath);
}

async function waitForIndexState(expected, previousGeneration = null) {
    const deadline = Date.now() + 30000;
    let lastDbError;
    while (Date.now() < deadline) {
        try {
            const db = openDb();
            const rows = Object.fromEntries(db.prepare(
                "SELECT key, value FROM kv WHERE key IN " +
                "('repo_root','index_state','index_generation')").all()
                .map(row => [row.key, row.value]));
            db.close();
            if (rows.index_state === expected &&
                rows.repo_root &&
                (previousGeneration === null ||
                 (rows.index_generation && rows.index_generation !== previousGeneration))) {
                return rows;
            }
        } catch (error) {
            // A short SQLite writer overlap is expected while the child commits.
            lastDbError = error;
        }
        await sleep(100);
    }
    throw new Error(
        `Timed out waiting for index_state=${expected}` +
        (lastDbError ? `; last SQLite error: ${lastDbError.message}` : ''));
}

async function waitForSettledInfo(client) {
    const deadline = Date.now() + 10000;
    let last;
    while (Date.now() < deadline) {
        last = await client.tool('server_info');
        if (last.index_status !== 'indexing') return last;
        await sleep(50);
    }
    throw new Error(`Timed out waiting for MCP index status to settle: ${JSON.stringify(last)}\n${client.logs}`);
}

async function waitForHealthStatus(client, expected) {
    const deadline = Date.now() + 10000;
    let last;
    while (Date.now() < deadline) {
        last = await client.tool('server_health');
        if (last.status === expected) return last;
        await sleep(50);
    }
    throw new Error(
        `Timed out waiting for health=${expected}: ${JSON.stringify(last)}\n${client.logs}`);
}

async function main() {
    assert(!fs.existsSync(base), `Refusing to overwrite existing fixture: ${base}`);
    fs.mkdirSync(path.join(primary, 'src'), { recursive: true });
    fs.mkdirSync(path.join(extra, 'src'), { recursive: true });
    created = true;
    fs.writeFileSync(
        path.join(primary, 'src', 'primary.cpp'),
        'int primary_only() { return 1; }\n');
    fs.writeFileSync(
        path.join(extra, 'src', 'extra.cpp'),
        'int extra_one() { return 1; }\nint extra_two() { return extra_one(); }\n');

    run([
        'index', '--root', primary, '--db', dbPath,
        '--threads', '1', '--arena-size', '8', '--batch-size', '1',
    ]);
    run(['workspace', 'add', extra, '--root', primary]);

    let db = openDb();
    const root = db.prepare('SELECT id FROM roots WHERE path = ?').get(extra);
    assert(root, 'Extra workspace root was not merged');
    const nodes = db.prepare(
        "SELECT n.id FROM files f INDEXED BY idx_files_root " +
        "CROSS JOIN nodes n INDEXED BY idx_nodes_file_id " +
        "WHERE f.root_id = ? AND n.file_id = f.id AND n.node_type = 'symbol' LIMIT 2")
        .all(root.id);
    assert.equal(nodes.length, 2, 'Expected two extra-root symbols');
    const sentinelEdge = 900000001;
    db.prepare(
        "INSERT INTO edges(id,src_id,dst_id,kind,confidence,evidence,source) " +
        "VALUES(?,?,?,?,?,?,?)")
        .run(sentinelEdge, nodes[0].id, nodes[1].id, 'calls', 1.0,
            'ownership-sentinel', 'runtime');
    const extraFilesBefore = db.prepare(
        'SELECT COUNT(*) AS count FROM files INDEXED BY idx_files_root WHERE root_id = ?')
        .get(root.id).count;
    db.exec(
        "DELETE FROM kv WHERE key IN " +
        "('repo_root','last_index_time','git_head','git_branch','index_state','index_generation')");
    db.close();

    const first = new Client();
    const second = new Client();
    await Promise.all([first.initialize(), second.initialize()]);
    const bootstrap = await waitForIndexState('needs_reconciliation');

    const [firstInfo, secondInfo] = await Promise.all([
        waitForSettledInfo(first), waitForSettledInfo(second),
    ]);
    const infoLogs = first.logs + second.logs;
    for (const info of [firstInfo, secondInfo]) {
        assert.equal(path.resolve(info.repo_root).toLowerCase(),
            path.resolve(primary).toLowerCase(), infoLogs);
        assert.equal(info.ownership_status, 'verified', infoLogs);
        assert.equal(info.index_status, 'needs_reconciliation', infoLogs);
        assert.equal(info.freshness_status, 'needs_reconciliation', infoLogs);
        assert.equal(info.stale, null, infoLogs);
    }
    assert.equal((await first.tool('server_health')).status, 'degraded');

    db = openDb();
    assert.equal(db.prepare(
        "SELECT COUNT(*) AS count FROM files WHERE path='src/primary.cpp' AND root_id IS NULL")
        .get().count, 1, 'Primary record was deleted during ownership bootstrap');
    assert.equal(db.prepare(
        'SELECT COUNT(*) AS count FROM files INDEXED BY idx_files_root WHERE root_id = ?')
        .get(root.id).count, extraFilesBefore);
    assert.equal(db.prepare('SELECT COUNT(*) AS count FROM edges WHERE id = ?')
        .get(sentinelEdge).count, 1, 'Extra-root graph edge was not preserved');
    db.close();

    const startupLogs = first.logs + second.logs;
    assert.equal((startupLogs.match(/reindex: started \(full\)/g) || []).length, 1,
        `Concurrent MCP clients duplicated startup work:\n${startupLogs}`);
    assert(startupLogs.includes(primary), `Child did not report the primary scan:\n${startupLogs}`);
    assert(!startupLogs.includes(`Scanning ${extra}`),
        `Child scanned the client CWD/additional root:\n${startupLogs}`);

    await first.tool('reindex');
    await waitForIndexState('current', bootstrap.index_generation);
    await waitForHealthStatus(first, 'ready');

    db = openDb();
    const writerLock = dbPath + '.lock';
    fs.writeFileSync(writerLock, String(process.pid), { flag: 'wx' });
    try {
        db.exec('BEGIN IMMEDIATE');
        db.prepare('UPDATE roots SET path = ? WHERE id = ?')
            .run(path.join(base, 'uncommitted-extra'), root.id);
        const started = Date.now();
        const listing = run(['workspace', 'list'], primary);
        assert(Date.now() - started < 3000, 'Read-only workspace listing waited for the writer');
        assert(listing.stdout.includes(extra), listing.stdout);
        assert(!listing.stdout.includes('uncommitted-extra'), listing.stdout);
        assert.match(listing.stdout, /Workspace roots \(2\)/);
        assert(listing.stdout.includes(primary), listing.stdout);
        assert(listing.stdout.includes('[primary]'), listing.stdout);
        assert(listing.stdout.includes('[additional]'), listing.stdout);
    } finally {
        db.exec('ROLLBACK');
        db.close();
        fs.unlinkSync(writerLock);
    }

    const logs = (await first.close()) + (await second.close());
    assert(!/ownership conflict|reindex: failed/.test(logs), logs);
    for (const client of [first, second]) {
        const logFile = path.join(primary, '.codetopo', 'logs', `mcp-${client.child.pid}.log`);
        const diagnostics = fs.readFileSync(logFile, 'utf8');
        assert(diagnostics.includes('stdio: ready; waiting for client initialize'), diagnostics);
        assert(diagnostics.includes('stdio: initialize received'), diagnostics);
        assert(diagnostics.includes('tool: server_info'), diagnostics);
        assert(diagnostics.includes('codetopo mcp stopped'), diagnostics);
    }
    console.log('PASS: authoritative root, non-destructive bootstrap, concurrent MCP coordination');
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
}).finally(async () => {
    for (const client of clients) {
        if (client.child.exitCode === null) client.child.kill();
    }
    await Promise.all(clients.map(client => client.closed));
    if (created) fs.rmSync(base, { recursive: true, force: true });
});
