// Node 22+. Uses only isolated repositories under the project build directory.
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const assert = require('node:assert/strict');
const { DatabaseSync } = require('node:sqlite');
const { createHash } = require('node:crypto');

const exe = path.resolve(process.argv[2]);
const base = path.join(process.cwd(), 'build', `cpp-workspace-stdio-${process.pid}`);
const primary = path.join(base, 'Primary');
const extra = path.join(base, 'Skiff');
const database = root => path.join(root, '.codetopo', 'index.sqlite');
const key = '1:src/execution/query_host.cpp::function::submit';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let server;
let releaseWriter;

function run(command, args, cwd = extra) {
    const result = spawnSync(command, args, { cwd, encoding: 'utf8', timeout: 60000 });
    assert.equal(result.status, 0, result.stdout + result.stderr + String(result.error || ''));
    return result.stdout.trim();
}

function open(root, readonly = false) {
    const db = new DatabaseSync(database(root), { readOnly: readonly });
    if (!readonly) db.function('codetopo_camel_split', { deterministic: true }, value => value);
    return db;
}

function primarySnapshot() {
    const db = open(primary, true);
    try {
        const files = db.prepare(
            'SELECT id,path,language,size_bytes,CAST(mtime_ns AS TEXT) AS mtime_ns,' +
            'content_hash,parse_status,parse_error,root_id FROM files WHERE root_id IS NULL ORDER BY id').all();
        const nodes = db.prepare(
            'SELECT n.id,n.stable_key,n.name,n.qualname,n.signature,n.start_line,n.end_line ' +
            'FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id ON n.file_id=f.id ' +
            'WHERE f.root_id IS NULL ORDER BY n.id').all();
        return createHash('sha256').update(JSON.stringify({ files, nodes })).digest('hex');
    } finally { db.close(); }
}

class Client {
    constructor() {
        this.child = spawn(exe, ['mcp', '--root', primary, '--db', database(primary), '--freshness=off'],
            { cwd: extra, stdio: ['pipe', 'pipe', 'pipe'] });
        this.logs = '';
        this.sequence = 0;
        this.pending = new Map();
        this.child.stderr.on('data', data => { this.logs += data; });
        this.closed = new Promise(resolve => this.child.once('close', code => resolve(code)));
        this.child.on('error', error => {
            for (const item of this.pending.values()) item.reject(error);
        });
        readline.createInterface({ input: this.child.stdout }).on('line', line => {
            const response = JSON.parse(line);
            if (response.id == null) return;
            const item = this.pending.get(response.id);
            assert(item, line);
            this.pending.delete(response.id);
            item.resolve(response);
        });
    }

    async request(method, params = {}) {
        const id = ++this.sequence;
        let timer;
        const response = new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            timer = setTimeout(() => reject(new Error(`RPC timed out: ${method}\n${this.logs}`)), 30000);
        });
        this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
        try { return await response; } finally { clearTimeout(timer); }
    }

    async tool(name, args = {}) {
        const response = await this.request('tools/call', { name, arguments: args });
        assert(!response.error, JSON.stringify(response));
        const value = JSON.parse(response.result.content[0].text);
        assert(!value.error, JSON.stringify(value));
        return value;
    }

    async waitJob(id) {
        const deadline = Date.now() + 60000;
        while (Date.now() < deadline) {
            const job = await this.tool('workspace_job_status', { job_id: id });
            if (['completed', 'failed', 'cancelled'].includes(job.status)) {
                assert.equal(job.status, 'completed', JSON.stringify(job) + this.logs);
                return job;
            }
            await delay(20);
        }
        throw new Error(`Job timed out\n${this.logs}`);
    }

    async close() {
        this.child.stdin.end();
        assert.equal(await this.closed, 0, this.logs);
    }
}

async function main() {
    fs.mkdirSync(path.join(primary, 'src'), { recursive: true });
    fs.mkdirSync(path.join(extra, 'src', 'execution'), { recursive: true });
    fs.mkdirSync(path.join(base, 'empty-hooks'));
    fs.writeFileSync(path.join(primary, 'src', 'primary.cpp'), 'int primary_stable() { return 7; }\n');
    const host = path.join(extra, 'src', 'execution', 'query_host.cpp');
    fs.writeFileSync(host,
        'namespace execution { struct QueryHost { void submit(int query,int context,int options) { int record=0; } }; }\n');
    fs.writeFileSync(path.join(extra, 'src', 'scheduler.cpp'),
        'namespace schedule { struct Scheduler { void submit(int task) {} }; }\n');
    fs.writeFileSync(path.join(extra, 'src', 'carrier.cpp'),
        'namespace carrier { template<class... Args> void submit(int backend,int task,Args&&... args) {} }\n');
    fs.writeFileSync(path.join(extra, 'src', 'callers.cpp'),
        'void scheduler_call(schedule::Scheduler& scheduler) { scheduler.submit(1); }\n' +
        'void carrier_call() { carrier::submit<int>(1,2,3); }\n' +
        'void host_call(execution::QueryHost& host) { auto handle=prepare(); host.submit(1,2,3); }\n');
    run('git', ['init']);
    run('git', ['config', 'user.name', 'Fixture']);
    run('git', ['config', 'user.email', 'fixture@example.invalid']);
    run('git', ['config', 'core.autocrlf', 'false']);
    run('git', ['config', 'core.hooksPath', path.join(base, 'empty-hooks')]);
    run('git', ['add', 'src']);
    run('git', ['commit', '-m', 'base']);
    const beforeCommit = run('git', ['rev-parse', 'HEAD']);
    fs.writeFileSync(host, fs.readFileSync(host, 'utf8').replace('record=0', 'record=1'));
    run('git', ['add', 'src']);
    run('git', ['commit', '-m', 'change']);
    const head = run('git', ['rev-parse', 'HEAD']);
    for (const root of [primary, extra])
        run(exe, ['index', '--root', root, '--threads', '1', '--no-gitignore']);
    const source = open(extra);
    const target = source.prepare("SELECT * FROM nodes WHERE stable_key='src/execution/query_host.cpp::function::submit'").get();
    const file = source.prepare("SELECT id FROM files WHERE path='src/execution/query_host.cpp'").get();
    source.prepare(
        'INSERT INTO nodes(node_type,file_id,kind,name,qualname,signature,start_line,end_line,stable_key) ' +
        "VALUES('symbol',?,'function','submit','submit',?,1,1,'src/execution/query_host.cpp::function::submit#2')")
        .run(file.id, target.signature);
    for (const caller of ['scheduler_call', 'carrier_call']) {
        const row = source.prepare('SELECT id FROM nodes WHERE name=?').get(caller);
        source.prepare("INSERT INTO edges(src_id,dst_id,kind,confidence,evidence) VALUES(?,?,'calls',0.75,'name-match')")
            .run(row.id, target.id);
    }
    source.close();
    const baseline = primarySnapshot();
    server = new Client();
    assert((await server.request('initialize')).result);
    server.child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    await server.waitJob((await server.tool('workspace_add', { path: extra })).job_id);
    const duplicate = await server.tool('impact_of', { file: host, symbol: 'submit', depth: 2 });
    assert.equal(duplicate.ambiguous, true, JSON.stringify(duplicate));
    const malformed = await server.request('tools/call',
        { name: 'workspace_refresh', arguments: { path: extra, reparse: 'yes' } });
    assert(malformed.error);
    const locked = open(extra);
    locked.exec('BEGIN IMMEDIATE');
    releaseWriter = () => { locked.exec('ROLLBACK'); locked.close(); releaseWriter = null; };
    const startLog = server.logs.length;
    const refresh = await server.tool('workspace_refresh', { path: extra, reparse: true });
    assert.equal(refresh.reparse, true);
    if (process.platform === 'win32') {
        const deadline = Date.now() + 10000;
        let worker;
        while (Date.now() < deadline && !worker) {
            worker = /\[child\] started pid=(\d+)/.exec(server.logs.slice(startLog));
            if (!worker) await delay(10);
        }
        assert(worker, server.logs);
        const command = run('powershell.exe', ['-NoProfile', '-Command',
            `(Get-CimInstance Win32_Process -Filter 'ProcessId=${worker[1]}').CommandLine`]);
        const option = name => {
            const match = new RegExp(`--${name}\\s+(?:"([^"]+)"|([^\\s]+))`).exec(command);
            assert(match, command);
            return match[1] || match[2];
        };
        assert.equal(path.resolve(option('root')).toLowerCase(), extra.toLowerCase());
        assert.equal(path.resolve(option('db')).toLowerCase(), database(extra).toLowerCase());
        assert(/(?:^|\s)--reparse(?:\s|$)/.test(command), command);
        assert(!/(?:^|\s)--force(?:\s|$)/.test(command), command);
    }
    releaseWriter();
    const job = await server.waitJob(refresh.job_id);
    assert.equal(job.result.root_id, 1);
    assert.equal(primarySnapshot(), baseline);
    for (const file of [host, process.platform === 'win32' ? host.toUpperCase() : host,
        extra + '/src\\execution/query_host.cpp']) {
        if (process.platform !== 'win32' && file.includes('\\')) continue;
        const impact = await server.tool('impact_of', { file, symbol: 'submit', depth: 2 });
        assert.equal(impact.symbol.stable_key, key);
        assert.equal(impact.symbol.qualname, 'execution::QueryHost::submit');
        assert(!impact.impacted.some(row => ['scheduler_call', 'carrier_call'].includes(row.name)), JSON.stringify(impact));
        assert(impact.impacted.some(row => row.name === 'host_call'), JSON.stringify(impact));
    }
    assert.equal((await server.tool('impact_of', { stable_key: key, depth: 2 })).symbol.stable_key, key);
    const changes = await server.tool('detect_changes', { repo_root: extra, since: beforeCommit, depth: 2 });
    assert.equal(changes.root_id, 1);
    assert.equal(changes.indexed_commit, head);
    assert.equal(changes.mapping_status, 'complete');
    assert(changes.changed_symbols.some(row => row.name === 'submit'));
    const verify = open(primary, true);
    assert.equal(verify.prepare(
        "SELECT COUNT(*) AS n FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id ON n.file_id=f.id " +
        "WHERE f.root_id=1 AND f.path LIKE '%/src/execution/query_host.cpp' AND n.name='submit'").get().n, 1);
    verify.close();
    await server.close();
    server = null;
    console.log('PASS: isolated CWD-B/root-A MCP repair, actual child root/db/reparse args, canonical selectors, stable handles, commit mapping, correct callers and unchanged primary lineage');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    if (releaseWriter) releaseWriter();
    if (server) await server.close();
    fs.rmSync(base, { recursive: true, force: true });
});
