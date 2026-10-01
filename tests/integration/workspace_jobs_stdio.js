// Node 22+ (built-in SQLite). Run from the repository with a Release executable.
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const assert = require('node:assert/strict');
const { DatabaseSync } = require('node:sqlite');
const { createHash } = require('node:crypto');

const exe = path.resolve(process.argv[2]);
const expectStartupBusy = process.argv.includes('--expect-startup-busy');
const acceptanceSequence = process.argv.includes('--acceptance-sequence');
const bulkOnly = process.argv.includes('--bulk-only') || process.argv.includes('--bulk-source-only') ||
    process.argv.includes('--bulk-after-primary');
const base = path.join(process.cwd(), acceptanceSequence
    ? '.codetopo-workspace-acceptance-test' : bulkOnly
    ? '.codetopo-workspace-bulk-stdio-test' : '.codetopo-workspace-stdio-test');
const primary = path.join(base, 'primary');
const extra = path.join(base, 'extra');
const sqlDir = path.join(extra, 'src', 'sql');
const primaryDb = path.join(primary, '.codetopo', 'index.sqlite');
const clients = [];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function assertHealthyChildren(logs) {
    const starts = [...logs.matchAll(/\[child\] started pid=(\d+)/g)];
    const exits = [...logs.matchAll(/\[child\] exited pid=(\d+)\s+(?:exit|wait_status)=(-?\d+)/g)];
    assert.equal(exits.length, starts.length, `Unfinished supervised children:\n${logs}`);
    assert.deepEqual(exits.map(exit => exit[1]).sort(), starts.map(start => start[1]).sort(),
        `Mismatched supervised child lifetimes:\n${logs}`);
    for (const exit of exits) assert.equal(Number(exit[2]), 0, `Failed supervised child:\n${logs}`);
    assert(!/Quarantine:|crash #|Restarting indexer|persist errors|file errors/.test(logs),
        `Unexpected recovery or lost files:\n${logs}`);
}

class Client {
    constructor(freshness = 'off', idle = null, watch = false, root = primary) {
        const args = ['mcp', '--root', root, '--db', path.join(root, '.codetopo', 'index.sqlite'), '--freshness', freshness];
        if (idle !== null) args.push('--idle-timeout', String(idle));
        if (watch) args.push('--watch', '--debounce', '50');
        this.child = spawn(exe, args,
        { stdio: ['pipe', 'pipe', 'pipe'] });
        this.sequence = 0;
        this.pending = new Map();
        this.logs = '';
        this.protocolError = null;
        this.exited = new Promise(resolve => {
            this.child.once('close', (code, signal) => resolve({ code, signal }));
            this.child.once('error', error => resolve({ error }));
        });
        this.child.stderr.on('data', data => { this.logs += data.toString(); });
        this.child.stdin.on('error', () => {});
        this.lines = readline.createInterface({ input: this.child.stdout });
        this.lines.on('line', line => {
            try {
                const message = JSON.parse(line);
                assert(message.id != null, `Unexpected protocol notification: ${line}`);
                const pending = this.pending.get(message.id);
                assert(pending, `Unknown response ID: ${line}`);
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

    async request(method, params) {
        const id = ++this.sequence;
        const message = { jsonrpc: '2.0', id, method };
        if (params) message.params = params;
        const response = new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
        });
        this.child.stdin.write(JSON.stringify(message) + '\n');
        let timer;
        try {
            return await Promise.race([response, new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error(`Request timeout: ${method}\n${this.logs.slice(-2000)}`)), 3000);
            })]);
        } finally {
            clearTimeout(timer);
        }
    }

    async initialize() {
        assert((await this.request('initialize')).result);
        this.child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    }

    tool(name, args = {}) { return this.request('tools/call', { name, arguments: args }); }

    payload(response) {
        assert(!response.error, JSON.stringify(response));
        const value = JSON.parse(response.result.content[0].text);
        assert(!value.error || value.status === 'failed', JSON.stringify(value));
        return value;
    }

    async waitJob(jobId) {
        const deadline = Date.now() + 90000;
        while (Date.now() < deadline) {
            const status = this.payload(await this.tool('workspace_job_status', { job_id: jobId }));
            if (['completed', 'failed', 'cancelled'].includes(status.status)) return status;
            const health = this.payload(await this.tool('server_health'));
            assert(['ready', 'busy'].includes(health.status));
            if (!expectStartupBusy) this.payload(await this.tool('server_info'));
            assert((await this.request('ping')).result);
            await sleep(25);
        }
        throw new Error('Workspace job exceeded 90 seconds');
    }

    async close(timeout = 90000) {
        if (!this.child.stdin.destroyed && !this.child.stdin.writableEnded) this.child.stdin.end();
        let timer;
        try {
            const outcome = await Promise.race([this.exited, new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error('Owned MCP child shutdown timed out')), timeout);
            })]);
            assert(!outcome.error, outcome.error);
            assert.equal(outcome.code, 0, this.logs.slice(-3000));
            assertHealthyChildren(this.logs);
            if (this.protocolError) throw this.protocolError;
            return this.logs;
        } finally {
            clearTimeout(timer);
        }
    }
}

async function waitUntil(predicate, label, timeout = 10000) {
    const deadline = Date.now() + timeout;
    while (!predicate() && Date.now() < deadline) await sleep(5);
    assert(predicate(), label);
}

// A real SQLite write transaction keeps the production child/merge in progress
// until every read has completed. No test-only server hooks or timing luck.
function holdWriter() {
    const db = new DatabaseSync(primaryDb);
    db.function('codetopo_camel_split', { deterministic: true }, value => value);
    db.exec('BEGIN IMMEDIATE');
    db.prepare("UPDATE nodes SET name='uncommittedName' WHERE name='primaryStable'").run();
    return () => { db.exec('ROLLBACK'); db.close(); };
}

async function committedReads(client, symbol = 'extra_0_0') {
    let maxMs = 0;
    const read = async (name, args = {}) => {
        const start = performance.now();
        const value = client.payload(await client.tool(name, args));
        const elapsed = performance.now() - start;
        maxMs = Math.max(maxMs, elapsed);
        assert(elapsed < 1000, `${name}: ${elapsed}ms`);
        return value;
    };
    const listing = await read('dir_list', { path: sqlDir, limit: 5 });
    assert.equal(listing.files.length, 5);
    const scoped = await read('symbols_in_path', { path: sqlDir, limit: 5, include_handles: true });
    assert(scoped.results.length > 0);
    const found = await read('symbol_search', { query: symbol, file_pattern: path.join(sqlDir, '*') });
    const selected = found.results.find(row => row.name === symbol && row.kind === 'function');
    assert(selected, JSON.stringify(found));
    const context = await read('context_for', { stable_key: selected.stable_key, include_source: false });
    assert.equal(context.symbol.name, symbol);
    const oldPrimary = await read('symbol_search', { query: 'primaryStable' });
    assert(oldPrimary.results.some(row => row.name === 'primaryStable'));
    assert.equal((await read('symbol_search', { query: 'uncommittedName' })).results.length, 0);
    await read('server_info');
    const stats = await read('repo_stats');
    assert.equal(stats.symbol_count, null);
    assert.equal(stats.edge_count, null);
    assert.equal(stats.graph_counts_checked, false);
    assert((await read('workspace_list')).roots.some(root => root.path === extra));
    return maxMs;
}

function checkQueryIndexes(dbPath) {
    const db = new DatabaseSync(dbPath);
    try {
        const indexes = new Set(db.prepare("SELECT name FROM sqlite_schema WHERE type='index'").all()
            .map(row => row.name));
        for (const name of ['idx_files_content_hash', 'idx_files_root', 'idx_nodes_file_id',
            'idx_nodes_type_kind_name', 'idx_nodes_qualname', 'idx_nodes_name_type',
            'idx_nodes_fingerprint', 'idx_nodes_stable_key', 'idx_refs_file_id',
            'idx_refs_kind_name', 'idx_refs_resolved', 'idx_refs_containing',
            'idx_edges_src', 'idx_edges_dst', 'idx_edges_dst_conf']) {
            assert(indexes.has(name), `Missing live query index: ${name}`);
        }
        const triggers = new Set(db.prepare("SELECT name FROM sqlite_schema WHERE type='trigger'").all()
            .map(row => row.name));
        for (const name of ['nodes_fts_insert', 'nodes_fts_delete', 'nodes_fts_update'])
            assert(triggers.has(name), `Missing live symbol FTS trigger: ${name}`);
        for (const query of [
            "SELECT n.id FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_file_id ON n.file_id=f.id WHERE f.path='main.c' AND n.node_type='symbol'",
            "SELECT n.id FROM nodes n INDEXED BY idx_nodes_name_type WHERE n.name='primaryStable' AND n.node_type='symbol'",
            "SELECT e.dst_id FROM edges e WHERE e.src_id IN (SELECT n.id FROM files f CROSS JOIN nodes n ON n.file_id=f.id WHERE f.path='main.c')",
            "SELECT e.src_id FROM edges e WHERE e.dst_id IN (SELECT n.id FROM files f CROSS JOIN nodes n ON n.file_id=f.id WHERE f.path='main.c')"
        ]) {
            const plan = db.prepare('EXPLAIN QUERY PLAN ' + query).all().map(row => row.detail);
            assert(!plan.some(detail => /^SCAN (nodes|edges|n|e)(?:\s|$)/.test(detail)), plan.join('\n'));
        }
    } finally { db.close(); }
}

async function allReadTools(client, file = path.join(sqlDir, 'source0.c'), symbol = 'extra_0_0') {
    const selected = client.payload(await client.tool('symbol_search', { query: symbol, file_pattern: file }))
        .results.find(row => row.name === symbol && row.kind === 'function');
    assert(selected, symbol);
    const selector = { stable_key: selected.stable_key };
    const calls = [
        ['server_info', {}], ['repo_stats', {}], ['server_health', {}], ['workspace_list', {}],
        ['dir_list', { path: path.dirname(file), limit: 5 }],
        ['dir_tree', { path: path.dirname(file), depth: 1, max_files: 5 }],
        ['file_search', { pattern: file, limit: 5 }],
        ['symbol_search', { query: '*', file_pattern: file, limit: 5 }],
        ['symbol_list', { file_path: file, limit: 5 }],
        ['symbols_in_path', { path: path.dirname(file), limit: 5 }],
        ['symbol_get_batch', { node_ids: [selected.node_id] }],
        ['file_summary', { path: file, limit: 5 }], ['file_overview', { path: file }],
        ['context_by_name', { name: symbol, file_pattern: file }],
        ['entrypoints', { scope: file, limit: 5 }],
        ['get_architecture', { scope: file, limit: 5 }],
        ['file_deps', { path: file, file_path: file }],
        ['subgraph', { seed_symbols: [selected.node_id], depth: 1 }],
        ['shortest_path', { src_id: selected.node_id, dst_id: selected.node_id,
            src_node_id: selected.node_id, dst_node_id: selected.node_id }],
        ['find_implementations', { symbol, limit: 5 }],
        ['dependency_cluster', { path: file }],
        ['source_at', { path: file, start_line: 1, end_line: 2 }],
        ['code_search', { query: symbol, file_pattern: file, limit: 5 }],
        ['list_http_calls', { file_pattern: file, limit: 5 }], ['get_traces', {}],
        ...['symbol_get', 'callers_approx', 'callees_approx', 'references', 'context_for',
            'impact_of', 'find_similar', 'method_fields'].map(name => [name, selector])
    ];
    for (const [name, args] of calls) {
        try {
            const response = await client.tool(name, args);
            const error = response.error || JSON.parse(response.result.content[0].text).error;
            if (['find_implementations', 'find_similar'].includes(name) && error) {
                // C functions are not base types; extractor rows may lack a fingerprint.
                assert.equal(error.data.error_code, 'not_found', JSON.stringify(response));
            } else {
                client.payload(response);
            }
        }
        catch (error) { throw new Error(`${name}: ${error.message}`, { cause: error }); }
    }
    // This fixture has no git history; exercising the prerequisite error still
    // ensures detect_changes is not rejected merely because indexing is active.
    const changes = await client.tool('detect_changes', { repo_root: path.dirname(file), since: 'HEAD' });
    const error = changes.error || JSON.parse(changes.result.content[0].text).error;
    if (error) assert(!['busy', 'db_error'].includes(error.data?.error_code), JSON.stringify(error));
    const nonReads = new Set(['reindex', 'ingest_traces', 'workspace_add', 'workspace_remove',
        'workspace_refresh', 'workspace_job_status', 'workspace_job_cancel', 'detect_changes']);
    for (const tool of (await client.request('tools/list')).result.tools) {
        assert(nonReads.has(tool.name) || calls.some(([name]) => name === tool.name),
            `Add bulk-overlap read coverage for ${tool.name}`);
    }
}

function writeBulk(root, generation) {
    const dir = path.join(root, 'bulk');
    fs.mkdirSync(dir, { recursive: true });
    for (let n = 0; n < 1100; n++) {
        fs.writeFileSync(path.join(dir, `file${String(n).padStart(4, '0')}.c`),
            Array.from({ length: 80 }, (_, i) =>
                `int bulk_${generation}_${n}_${i}() { return ${i}; }\n`).join(''));
    }
}

// Pause only after a production child has committed a real bulk batch, not
// while it is still waiting to open the DB. The last bulk file must remain old.
async function holdBulkWriter(client, logStart, dbPath, lastHash) {
    await waitUntil(() => client.logs.slice(logStart).includes('Bulk mode:'),
        'Production child did not enter >1000-file bulk mode', 90000);
    const db = new DatabaseSync(dbPath);
    const deadline = Date.now() + 90000;
    try {
        while (Date.now() < deadline) {
            if (fs.existsSync(dbPath + '.progress')) {
                const progress = fs.readFileSync(dbPath + '.progress', 'utf8').trim();
                if (progress && progress !== 'bulk/file1099.c') {
                    try { db.exec('BEGIN IMMEDIATE'); }
                    catch (error) {
                        if (!/locked|busy/.test(error.message)) throw error;
                        await sleep(1);
                        continue;
                    }
                    assert.equal(db.prepare("SELECT content_hash FROM files WHERE path='bulk/file1099.c'")
                        .get()?.content_hash ?? null, lastHash, 'Bulk persistence already completed');
                    assert(!client.logs.slice(logStart).includes('Resolved '),
                        'Must pause during actual bulk persistence, before reference resolution');
                    return () => { db.exec('ROLLBACK'); db.close(); };
                }
            }
            await sleep(1);
        }
        throw new Error('Could not overlap a committed production bulk batch');
    } catch (error) {
        if (db.isTransaction) db.exec('ROLLBACK');
        db.close();
        throw error;
    }
}

function bulkLastHash(dbPath) {
    const db = new DatabaseSync(dbPath);
    try {
        return db.prepare("SELECT content_hash FROM files WHERE path='bulk/file1099.c'")
            .get()?.content_hash ?? null;
    } finally { db.close(); }
}

function assertPrimaryBulkComplete(generation) {
    const db = new DatabaseSync(primaryDb);
    try {
        assert.equal(db.prepare("SELECT COUNT(*) AS n FROM quarantine").get().n, 0);
        assert.equal(db.prepare("SELECT COUNT(*) AS n FROM files WHERE root_id IS NULL AND path GLOB 'bulk/*.c'").get().n, 1100);
        assert.equal(db.prepare(`SELECT COUNT(DISTINCT n.name) AS n FROM files f CROSS JOIN nodes n ON n.file_id=f.id
            WHERE f.root_id IS NULL AND f.path GLOB 'bulk/*.c' AND n.kind='function' AND n.name GLOB ?`)
            .get(`bulk_${generation}_*`).n, 88000);
        assert.equal(db.prepare('SELECT COUNT(*) AS n FROM content_fts WHERE content_fts MATCH ?')
            .get(`bulk_${generation}_1099_79`).n, 1);
    } finally { db.close(); }
}

async function waitPrimaryBulkComplete(client, generation, logStart) {
    const deadline = Date.now() + 90000;
    while (Date.now() < deadline) {
        const db = new DatabaseSync(primaryDb);
        let names;
        try {
            names = db.prepare(`SELECT COUNT(DISTINCT n.name) AS n FROM files f CROSS JOIN nodes n ON n.file_id=f.id
                WHERE f.root_id IS NULL AND f.path GLOB 'bulk/*.c' AND n.kind='function' AND n.name GLOB ?`)
                .get(`bulk_${generation}_*`).n;
        } finally { db.close(); }
        if (names === 88000 && client.payload(await client.tool('server_health')).status === 'ready') {
            assertPrimaryBulkComplete(generation);
            return;
        }
        await sleep(50);
    }
    throw new Error(`Bulk ${generation} did not retain all expected contents:\n${client.logs.slice(logStart)}`);
}

async function committedBulkSearch(client, root, generation) {
    const symbol = `bulk_${generation}_0_0`;
    const found = client.payload(await client.tool('symbol_search', {
        query: symbol, file_pattern: path.join(root, 'bulk', 'file0000.c')
    }));
    assert(found.results.some(row => row.name === symbol),
        `Committed bulk batch must already be symbol-searchable: ${symbol}`);
}

async function bulkOverlapReads() {
    // Source refresh owns a distinct DB. Validate that writer separately before
    // primary bulk tests, which deliberately churn the primary graph's IDs.
    if (!process.argv.includes('--bulk-after-primary')) {
        const sourceReader = new Client();
        await sourceReader.initialize();
        await bulkSourceReads(sourceReader);
        await sourceReader.close();
    }
    if (process.argv.includes('--bulk-source-only')) {
        return;
    }
    writeBulk(primary, 'startup');
    let client = new Client('normal', null, true);
    await client.initialize();
    let release = await holdBulkWriter(client, 0, primaryDb, null);
    try {
        await committedReads(client);
        await allReadTools(client);
        await committedBulkSearch(client, primary, 'startup');
        checkQueryIndexes(primaryDb);
        assert.equal(client.payload(await client.tool('server_health')).status, 'busy');
        console.log('PASS: >1000-file normal startup --watch, committed bulk batch, every read tool and bounded plans');
    } finally { release(); }
    await waitUntil(() => client.logs.includes('reindex: done'), 'Bulk startup did not finish', 90000);
    assertPrimaryBulkComplete('startup');
    if (process.argv.includes('--bulk-after-primary')) {
        await bulkSourceReads(client);
        await client.close();
        return;
    }

    const beforeWatch = bulkLastHash(primaryDb);
    const watchLog = client.logs.length;
    writeBulk(primary, 'watch');
    release = await holdBulkWriter(client, watchLog, primaryDb, beforeWatch);
    try {
        await committedReads(client);
        await allReadTools(client);
        await committedBulkSearch(client, primary, 'watch');
        checkQueryIndexes(primaryDb);
        assert(client.logs.slice(watchLog).includes('Targeted scan:'), client.logs.slice(watchLog));
        console.log('PASS: >1000-file real watcher targeted child, committed bulk batch, every read tool');
    } finally { release(); }
    await waitUntil(() => client.logs.slice(watchLog).includes('reindex: done'), 'Bulk watch did not finish', 90000);
    // The real watcher can coalesce another targeted child while writes arrive.
    // One child's "done" is not completion of the whole filesystem generation.
    await waitPrimaryBulkComplete(client, 'watch', watchLog);
    await client.close();

    client = new Client();
    await client.initialize();
    const beforeExplicit = bulkLastHash(primaryDb);
    writeBulk(primary, 'explicit');
    const explicitLog = client.logs.length;
    assert.equal(client.payload(await client.tool('reindex')).status, 'started');
    release = await holdBulkWriter(client, explicitLog, primaryDb, beforeExplicit);
    try {
        await committedReads(client);
        await allReadTools(client);
        await committedBulkSearch(client, primary, 'explicit');
        checkQueryIndexes(primaryDb);
        console.log('PASS: >1000-file explicit MCP reindex, committed bulk batch, every read tool');
    } finally { release(); }
    await waitUntil(() => client.logs.slice(explicitLog).includes('reindex: done'), 'Bulk explicit child did not finish', 90000);
    assertPrimaryBulkComplete('explicit');

    for (const mode of ['cli', 'targeted-cli', 'force-cli']) {
        const lastHash = mode === 'force-cli' ? null : bulkLastHash(primaryDb);
        writeBulk(primary, mode.replaceAll('-', '_'));
        const args = ['index', '--root', primary, '--threads', '2', '--arena-size', '8', '--batch-size', '100'];
        const changedList = path.join(primary, '.codetopo', 'bulk-files.lst');
        if (mode === 'targeted-cli') {
            fs.writeFileSync(changedList, Array.from({ length: 1100 }, (_, n) =>
                `bulk/file${String(n).padStart(4, '0')}.c\n`).join(''));
            args.push('--changed-file', changedList);
        }
        if (mode === 'force-cli') args.push('--force');
        const process = spawn(exe, args, { stdio: ['ignore', 'pipe', 'pipe'] });
        const writer = { logs: '' };
        process.stdout.on('data', data => { writer.logs += data; });
        process.stderr.on('data', data => { writer.logs += data; });
        const exited = new Promise(resolve => {
            process.once('close', code => resolve({ code }));
            process.once('error', error => resolve({ error }));
        });
        try {
            release = await holdBulkWriter(writer, 0, primaryDb, lastHash);
            try {
                await allReadTools(client);
                await committedBulkSearch(client, primary, mode.replaceAll('-', '_'));
                checkQueryIndexes(primaryDb);
                assert.equal(process.exitCode, null, 'CLI writer must still be active');
                console.log(`PASS: >1000-file ${mode}, committed bulk batch, every read tool`);
            } finally { release(); }
        } finally {
            const result = await exited;
            assert(!result.error && result.code === 0, writer.logs + JSON.stringify(result));
            assertHealthyChildren(writer.logs);
            assertPrimaryBulkComplete(mode.replaceAll('-', '_'));
            if (fs.existsSync(changedList)) fs.unlinkSync(changedList);
        }
    }

    await client.close();
}

async function bulkSourceReads(client) {
    const sourceDb = path.join(extra, '.codetopo', 'index.sqlite');
    const sourceClient = new Client('off', null, false, extra);
    await sourceClient.initialize();
    writeBulk(extra, 'source');
    const refreshLog = client.logs.length;
    const accepted = client.payload(await client.tool('workspace_refresh', { path: extra }));
    const release = await holdBulkWriter(client, refreshLog, sourceDb, null);
    try {
        await committedReads(client);
        await allReadTools(client);
        await allReadTools(sourceClient);
        await committedBulkSearch(sourceClient, extra, 'source');
        checkQueryIndexes(primaryDb);
        checkQueryIndexes(sourceDb);
        assert.equal(client.payload(await client.tool('workspace_job_status', { job_id: accepted.job_id })).phase,
            'indexing_extra_root');
    } finally { release(); }
    const result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result) + '\n' +
        client.logs.split(/\r?\n/).filter(line => /SQLite|failed|exited|ERROR|Fatal|crash|Quarantine/.test(line)).join('\n') +
        '\n' + client.logs.slice(-3000));
    assertHealthyChildren(client.logs.slice(refreshLog));
    for (const dbPath of [sourceDb, primaryDb]) {
        const db = new DatabaseSync(dbPath);
        try {
            assert.equal(db.prepare('SELECT COUNT(*) AS n FROM quarantine').get().n, 0);
            const scope = dbPath === sourceDb ? 'root_id IS NULL' : 'root_id IS NOT NULL';
            assert.equal(db.prepare(`SELECT COUNT(*) AS n FROM files WHERE ${scope}`).get().n, 1220);
            assert.equal(db.prepare(`SELECT COUNT(DISTINCT n.name) AS n FROM files f CROSS JOIN nodes n ON n.file_id=f.id
                WHERE f.${scope} AND n.kind='function' AND n.name GLOB 'bulk_source_*'`).get().n, 88000);
            if (dbPath === sourceDb)
                assert.equal(db.prepare("SELECT COUNT(*) AS n FROM content_fts WHERE content_fts MATCH 'bulk_source_1099_79'").get().n, 1);
        } finally { db.close(); }
    }
    const sourceRows = new DatabaseSync(sourceDb);
    const mergedRows = new DatabaseSync(primaryDb);
    try {
        const expected = sourceRows.prepare("SELECT path,content_hash,CAST(mtime_ns AS TEXT) AS mtime FROM files WHERE root_id IS NULL")
            .all().map(row => ({ ...row, path: path.join(extra, row.path).replaceAll('\\', '/') }))
            .sort((a, b) => a.path.localeCompare(b.path));
        const actual = mergedRows.prepare("SELECT path,content_hash,CAST(mtime_ns AS TEXT) AS mtime FROM files WHERE root_id IS NOT NULL")
            .all().map(row => ({ ...row, path: row.path.replaceAll('\\', '/') }))
            .sort((a, b) => a.path.localeCompare(b.path));
        assert.deepEqual(actual, expected, 'Every merged file must match its committed source contents');
    } finally { sourceRows.close(); mergedRows.close(); }
    assert(sourceClient.payload(await sourceClient.tool('symbol_search', { query: 'bulk_source_1099_0' })).results.length);
    await sourceClient.close();
    console.log('PASS: >1000-file workspace refresh, primary/source reads during bulk persistence; all children exit 0, no quarantine, all 1100 files and 88000 function names retained');
}

async function index(root) {
    const child = spawn(exe, ['index', '--root', root, '--threads', '2', '--arena-size', '8'],
        { stdio: ['ignore', 'pipe', 'pipe'] });
    let log = '';
    child.stdout.on('data', data => { log += data.toString(); });
    child.stderr.on('data', data => { log += data.toString(); });
    const timer = setTimeout(() => child.kill(), 90000);
    try {
        const result = await new Promise((resolve, reject) => {
            child.once('exit', code => resolve(code));
            child.once('error', reject);
        });
        assert.equal(result, 0, log);
        assertHealthyChildren(log);
    } finally {
        clearTimeout(timer);
    }
}

function withDb(fn) {
    const db = new DatabaseSync(primaryDb);
    try { return fn(db); } finally { db.close(); }
}

function symbols(rootId = null) {
    return withDb(db => new Set(db.prepare(
        'SELECT n.name FROM files f CROSS JOIN nodes n ON n.file_id=f.id WHERE f.root_id IS ?'
    ).all(rootId).map(row => row.name)));
}

function primarySnapshot() {
    return withDb(db => ({
        files: db.prepare('SELECT id,path,content_hash,CAST(mtime_ns AS TEXT) AS mtime_ns FROM files WHERE root_id IS NULL').all(),
        indexed: db.prepare("SELECT value FROM kv WHERE key='last_index_time'").get()
    }));
}

function seedLegacyWorkspaceIds(rootId) {
    const db = new DatabaseSync(primaryDb);
    const offset = rootId * 1000000000;
    db.function('codetopo_camel_split', { deterministic: true }, value => {
        if (value === null) return null;
        const split = value.replace(/([a-z])([A-Z])/g, '$1 $2').replace(/([A-Z])([A-Z][a-z])/g, '$1 $2');
        return split === value ? value : value + ' ' + split;
    });
    try {
        db.exec('BEGIN IMMEDIATE; PRAGMA defer_foreign_keys=ON');
        db.exec('CREATE TEMP TABLE fixture_nodes(id INTEGER PRIMARY KEY)');
        db.exec(`INSERT INTO fixture_nodes
            SELECT n.id FROM files f CROSS JOIN nodes n ON n.file_id=f.id WHERE f.root_id=${rootId}
            UNION SELECT id FROM nodes INDEXED BY idx_nodes_stable_key
            WHERE stable_key >= '${rootId}:' AND stable_key < '${rootId};' AND node_type='file'`);
        for (const table of ['files', 'nodes', 'refs']) {
            db.exec(`CREATE TEMP TABLE fixture_${table}_map(id INTEGER PRIMARY KEY, legacy INTEGER UNIQUE)`);
            const scope = table === 'files' ? `SELECT id FROM files WHERE root_id=${rootId}` :
                table === 'nodes' ? 'SELECT id FROM fixture_nodes' :
                `SELECT r.id FROM files f CROSS JOIN refs r ON r.file_id=f.id WHERE f.root_id=${rootId}`;
            db.exec(`INSERT INTO fixture_${table}_map SELECT id,${offset}+ROW_NUMBER() OVER(ORDER BY id) FROM (${scope})`);
        }
        // Reproduce the old namespace without changing graph contents or primary rows.
        db.exec(`UPDATE edges SET
            src_id=COALESCE((SELECT legacy FROM fixture_nodes_map WHERE id=src_id),src_id),
            dst_id=COALESCE((SELECT legacy FROM fixture_nodes_map WHERE id=dst_id),dst_id)
            WHERE src_id IN (SELECT id FROM fixture_nodes_map) OR dst_id IN (SELECT id FROM fixture_nodes_map)`);
        db.exec(`UPDATE refs SET
            id=COALESCE((SELECT legacy FROM fixture_refs_map WHERE id=refs.id),id),
            file_id=COALESCE((SELECT legacy FROM fixture_files_map WHERE id=file_id),file_id),
            resolved_node_id=COALESCE((SELECT legacy FROM fixture_nodes_map WHERE id=resolved_node_id),resolved_node_id),
            containing_node_id=COALESCE((SELECT legacy FROM fixture_nodes_map WHERE id=containing_node_id),containing_node_id)
            WHERE file_id IN (SELECT id FROM fixture_files_map)
                OR resolved_node_id IN (SELECT id FROM fixture_nodes_map)
                OR containing_node_id IN (SELECT id FROM fixture_nodes_map)`);
        db.exec(`UPDATE nodes SET id=(SELECT legacy FROM fixture_nodes_map WHERE id=nodes.id),
            file_id=(SELECT legacy FROM fixture_files_map WHERE id=file_id)
            WHERE id IN (SELECT id FROM fixture_nodes_map)`);
        db.exec(`UPDATE files SET id=(SELECT legacy FROM fixture_files_map WHERE id=files.id)
            WHERE id IN (SELECT id FROM fixture_files_map)`);
        db.exec('COMMIT');
    } finally {
        if (db.isTransaction) db.exec('ROLLBACK');
        db.close();
    }
}

function initializeAcceptanceRepo(root) {
    const result = spawnSync('git', ['init', '--quiet', '--initial-branch=cristian/2026/10/01/acceptance-fixture', root],
        { encoding: 'utf8' });
    assert(!result.error, result.error?.message);
    assert.equal(result.status, 0, result.stderr);
}

function acceptancePrimaryState() {
    return withDb(db => {
        db.exec('CREATE TEMP TABLE owned_nodes(id INTEGER PRIMARY KEY)');
        db.exec(`INSERT INTO owned_nodes
            SELECT n.id FROM files f CROSS JOIN nodes n ON n.file_id=f.id WHERE f.root_id IS NULL
            UNION SELECT n.id FROM files f CROSS JOIN nodes n INDEXED BY idx_nodes_stable_key
            ON n.stable_key=f.path||'::file' WHERE f.root_id IS NULL`);
        const digest = sql => createHash('sha256').update(JSON.stringify(db.prepare(sql).all())).digest('hex');
        return {
            files: digest("SELECT id,path,content_hash,CAST(mtime_ns AS TEXT) AS mtime FROM files WHERE root_id IS NULL ORDER BY id"),
            nodes: digest('SELECT n.* FROM owned_nodes o CROSS JOIN nodes n ON n.id=o.id ORDER BY n.id'),
            refs: digest('SELECT r.* FROM files f CROSS JOIN refs r ON r.file_id=f.id WHERE f.root_id IS NULL ORDER BY r.id'),
            edges: digest('SELECT e.* FROM edges e WHERE src_id IN (SELECT id FROM owned_nodes) AND dst_id IN (SELECT id FROM owned_nodes) ORDER BY e.id')
        };
    });
}

function startAcceptanceTraffic(client, primaryKey) {
    const state = { stop: false, failure: null, rounds: 0 };
    const running = (async () => {
        while (!state.stop) {
            const responses = await Promise.all(Array.from({ length: 8 }, () => [
                client.tool('symbol_search', { query: 'primaryStable', file_pattern: path.join(primary, 'main.c') }),
                client.tool('context_for', { stable_key: primaryKey, include_source: false }),
                client.tool('dir_list', { path: primary, limit: 5 })
            ]).flat());
            for (const response of responses) client.payload(response);
            state.rounds++;
            await sleep(10);
        }
    })().catch(error => { state.failure = error; });
    return async () => {
        state.stop = true;
        await running;
        assert.ifError(state.failure);
        assert(state.rounds > 0, 'Cache traffic did not overlap the transition');
        return state.rounds;
    };
}

async function connectedAcceptance(rootId) {
    seedLegacyWorkspaceIds(rootId);
    writeBulk(primary, 'acceptance');
    fs.writeFileSync(path.join(primary, 'growth.c'),
        'int primaryLocalTarget() { return 9; }\nint namespaceGrowthCaller() { return primaryLocalTarget(); }\n');
    const client = new Client('normal', null, true);
    await client.initialize();
    const select = async (name, file) => {
        const found = client.payload(await client.tool('symbol_search', { query: name, file_pattern: file }));
        const result = found.results.find(row => row.name === name && row.kind === 'function');
        assert(result, JSON.stringify(found));
        return result;
    };
    const writersReady = async () => {
        const deadline = Date.now() + 90000;
        while (Date.now() < deadline) {
            const health = client.payload(await client.tool('server_health'));
            const starts = client.logs.match(/\[child\] started pid=\d+/g) || [];
            const exits = client.logs.match(/\[child\] exited pid=\d+\s+(?:exit|wait_status)=-?\d+/g) || [];
            if (health.status === 'ready' && starts.length === exits.length) {
                assertHealthyChildren(client.logs);
                return;
            }
            await sleep(25);
        }
        throw new Error(`Production writers did not finish:\n${client.logs}`);
    };
    const primaryHandle = await select('primaryStable', path.join(primary, 'main.c'));
    const oldExtraHandle = await select('extra_0_0', path.join(sqlDir, 'source0.c'));
    let release = await holdBulkWriter(client, 0, primaryDb, null);
    let stopTraffic = startAcceptanceTraffic(client, primaryHandle.stable_key);
    try {
        assert.equal(client.payload(await client.tool('server_health')).status, 'busy');
        await committedReads(client);
        await allReadTools(client);
        await allReadTools(client, path.join(primary, 'main.c'), 'primaryStable');
        await committedBulkSearch(client, primary, 'acceptance');
        checkQueryIndexes(primaryDb);
        assert(!client.logs.includes('reindex: done'), 'Startup writer finished before read checks');
    } finally { release(); }
    try { await waitPrimaryBulkComplete(client, 'acceptance', 0); }
    finally { await stopTraffic(); }
    await writersReady();
    const preservedPrimary = acceptancePrimaryState();
    const preservedMetadata = primarySnapshot();
    assert(withDb(db => db.prepare("SELECT MIN(id) AS id FROM files WHERE root_id IS NULL AND path GLOB 'bulk/*.c'").get().id)
        > rootId * 1000000000, 'Primary growth did not cross the existing legacy workspace namespace');
    assert(withDb(db => db.prepare(`SELECT MIN(r.id) AS id FROM files f CROSS JOIN refs r ON r.file_id=f.id
        WHERE f.root_id IS NULL AND f.path='growth.c'`).get().id) > rootId * 1000000000,
        'Primary reference growth did not cross the legacy reference namespace');
    console.log('CHECKPOINT: same normal --watch session served every safe read during committed >1000-file startup; primary file/node/reference growth crossed the legacy namespace and cache traffic survived completion');

    const second = path.join(base, 'second');
    fs.mkdirSync(second);
    fs.writeFileSync(path.join(second, 'second.c'),
        'int primaryStable();\nint secondRootSymbol() { return primaryStable(); }\n');
    initializeAcceptanceRepo(second);
    let accepted = client.payload(await client.tool('workspace_add', { path: second }));
    let result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    await writersReady();
    const secondId = result.result.root_id;
    assert.notEqual(secondId, rootId);
    const secondHandle = await select('secondRootSymbol', path.join(second, 'second.c'));
    assert.deepEqual(acceptancePrimaryState(), preservedPrimary);
    assert.deepEqual(primarySnapshot(), preservedMetadata);
    const unchangedPrimaryHandle = await select('primaryStable', path.join(primary, 'main.c'));
    assert.equal(unchangedPrimaryHandle.node_id, primaryHandle.node_id);
    console.log('CHECKPOINT: second-root add completed after primary growth without changing primary files, nodes, refs, intrinsic edges, handles, or metadata');

    const sourceDb = path.join(extra, '.codetopo', 'index.sqlite');
    const sourceReader = new Client('off', null, false, extra);
    await sourceReader.initialize();
    writeBulk(extra, 'source');
    fs.writeFileSync(path.join(sqlDir, 'source0.c'), 'int refreshedOnly() { return 7; }\n');
    fs.unlinkSync(path.join(sqlDir, 'source1.c'));
    fs.writeFileSync(path.join(sqlDir, 'added.c'),
        'int refreshedOnly();\nint newlyAdded() { return refreshedOnly(); }\n');
    const refreshLog = client.logs.length;
    accepted = client.payload(await client.tool('workspace_refresh', { path: extra }));
    release = await holdBulkWriter(client, refreshLog, sourceDb, null);
    stopTraffic = startAcceptanceTraffic(client, primaryHandle.stable_key);
    try {
        await committedReads(client);
        await allReadTools(client);
        await allReadTools(sourceReader);
        await committedBulkSearch(sourceReader, extra, 'source');
        checkQueryIndexes(primaryDb);
        checkQueryIndexes(sourceDb);
        assert.equal(client.payload(await client.tool('workspace_job_status', { job_id: accepted.job_id })).phase,
            'indexing_extra_root');
    } finally { release(); }
    try {
        result = await client.waitJob(accepted.job_id);
        assert.equal(result.status, 'completed', JSON.stringify(result));
    } finally { await stopTraffic(); }
    await writersReady();
    assert.equal(result.result.root_id, rootId);
    assert.deepEqual(acceptancePrimaryState(), preservedPrimary);
    assert.deepEqual(primarySnapshot(), preservedMetadata);
    assert.equal((await select('secondRootSymbol', path.join(second, 'second.c'))).node_id, secondHandle.node_id);
    assert.equal((await select('primaryStable', path.join(primary, 'main.c'))).node_id, primaryHandle.node_id);
    const stale = await client.tool('context_for', { stable_key: oldExtraHandle.stable_key, include_source: false });
    const staleError = stale.error || JSON.parse(stale.result.content[0].text).error;
    assert.equal(staleError?.code, -32602, JSON.stringify(stale));
    assert.equal(staleError?.data?.error_code, 'invalid_input', JSON.stringify(stale));
    assert.equal(staleError?.message, `stable_key not found: ${oldExtraHandle.stable_key}`);
    await select('refreshedOnly', path.join(sqlDir, 'source0.c'));
    assert.equal(client.payload(await client.tool('symbol_search', { query: 'extra_1_0' })).results.length, 0);
    const source = new DatabaseSync(sourceDb);
    try {
        const expected = source.prepare("SELECT path,content_hash,CAST(mtime_ns AS TEXT) AS mtime FROM files WHERE root_id IS NULL")
            .all().map(row => ({ ...row, path: path.join(extra, row.path).replaceAll('\\', '/') }))
            .sort((a, b) => a.path.localeCompare(b.path));
        const actual = withDb(db => db.prepare("SELECT path,content_hash,CAST(mtime_ns AS TEXT) AS mtime FROM files WHERE root_id=?")
            .all(rootId)).map(row => ({ ...row, path: row.path.replaceAll('\\', '/') }))
            .sort((a, b) => a.path.localeCompare(b.path));
        assert.equal(expected.length, 1220);
        assert.deepEqual(actual, expected);
        assert.equal(source.prepare('SELECT COUNT(*) AS n FROM quarantine').get().n, 0);
        assert.equal(source.prepare(`SELECT COUNT(DISTINCT n.name) AS n FROM files f CROSS JOIN nodes n ON n.file_id=f.id
            WHERE f.root_id IS NULL AND n.kind='function' AND n.name GLOB 'bulk_source_*'`).get().n, 88000);
        assert.equal(withDb(db => db.prepare(`SELECT COUNT(DISTINCT n.name) AS n FROM files f CROSS JOIN nodes n ON n.file_id=f.id
            WHERE f.root_id=? AND n.kind='function' AND n.name GLOB 'bulk_source_*'`).get(rootId).n), 88000);
        assert.equal(source.prepare("SELECT COUNT(*) AS n FROM content_fts WHERE content_fts MATCH 'bulk_source_1099_79'").get().n, 1);
    } finally { source.close(); }
    withDb(db => {
        const calls = db.prepare(`SELECT r.containing_node_id,r.resolved_node_id,c.name AS caller,t.name AS target
            FROM files f CROSS JOIN refs r ON r.file_id=f.id
            LEFT JOIN nodes c ON c.id=r.containing_node_id LEFT JOIN nodes t ON t.id=r.resolved_node_id
            WHERE f.root_id=? AND r.kind='call' AND r.name='refreshedOnly'`).all(rootId);
        assert(calls.length > 0, 'Refreshed source lost its call references');
        for (const call of calls) {
            assert.equal(call.caller, 'newlyAdded');
            assert.equal(call.target, 'refreshedOnly');
            assert(db.prepare("SELECT 1 FROM edges WHERE src_id=? AND dst_id=? AND kind='calls' LIMIT 1")
                .get(call.containing_node_id, call.resolved_node_id), 'Refreshed call edge disagrees with reference IDs');
        }
    });
    await sourceReader.close();
    console.log('CHECKPOINT: original-root bulk refresh preserved the primary and second root, copied all source file contents and 88000 bulk names, remapped call reference/edge endpoints, and rejected the stale cached selector explicitly');

    release = holdWriter();
    accepted = client.payload(await client.tool('workspace_remove', { path: extra }));
    try {
        await waitUntil(() => client.logs.includes(`workspace job: ${accepted.job_id} phase=removing`),
            'Workspace remove did not enter its writer phase');
        await committedReads(client, 'refreshedOnly');
        await allReadTools(client, path.join(sqlDir, 'source0.c'), 'refreshedOnly');
    } finally { release(); }
    result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    await writersReady();
    assert.deepEqual(acceptancePrimaryState(), preservedPrimary);
    assert.deepEqual(primarySnapshot(), preservedMetadata);
    assert.equal((await select('secondRootSymbol', path.join(second, 'second.c'))).node_id, secondHandle.node_id);
    accepted = client.payload(await client.tool('workspace_remove', { path: second }));
    result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    await writersReady();
    assert.deepEqual(client.payload(await client.tool('workspace_list')).roots, []);
    for (const id of [rootId, secondId]) {
        assert.equal(withDb(db => db.prepare('SELECT COUNT(*) AS n FROM files WHERE root_id=?').get(id).n), 0);
        assert.equal(withDb(db => db.prepare('SELECT COUNT(*) AS n FROM nodes INDEXED BY idx_nodes_stable_key WHERE stable_key>=? AND stable_key<?')
            .get(`${id}:`, `${id};`).n), 0);
    }
    assert.deepEqual(acceptancePrimaryState(), preservedPrimary);
    console.log('CHECKPOINT: both workspace roots removed; committed reads worked during blocked removal and primary data survived');

    release = holdWriter();
    const eofLog = client.logs.length;
    try {
        assert.equal(client.payload(await client.tool('reindex')).status, 'started');
        await waitUntil(() => client.logs.slice(eofLog).includes('[child] started pid='),
            'EOF regression has no active primary child');
        assert.equal(client.payload(await client.tool('server_health')).status, 'busy');
        assert.equal(client.child.exitCode, null);
        client.child.stdin.end();
        await waitUntil(() => client.logs.slice(eofLog).includes('shutdown: draining index child'),
            'EOF did not drain the active child');
    } finally { release(); }
    await client.close();
    assertHealthyChildren(client.logs);
    assert.deepEqual(acceptancePrimaryState(), preservedPrimary);
    assert.equal(withDb(db => db.prepare('SELECT COUNT(*) AS n FROM quarantine').get().n), 0);
    for (const db of [primaryDb, sourceDb, path.join(second, '.codetopo', 'index.sqlite')]) {
        const check = new DatabaseSync(db);
        try { assert.equal(check.prepare('SELECT COUNT(*) AS n FROM quarantine').get().n, 0); }
        finally { check.close(); }
        for (const suffix of ['.lock', '.progress', '.worklist']) assert(!fs.existsSync(db + suffix), db + suffix);
    }
    const childEvidence = client.logs.match(/\[child\] exited pid=\d+\s+(?:exit|wait_status)=-?\d+(?:\s+elapsed_ms=\d+)?/g) || [];
    for (const line of childEvidence) console.log(line.replace(/\s+/g, ' '));
    console.log('PASS: connected legacy extra root -> normal --watch bulk startup/read traffic -> primary growth -> second-root add -> bulk refresh/cache transitions -> remove both roots -> EOF during active child; all children exit 0, zero quarantine, 1100 files/88000 names per bulk, exact primary/source contents and primary handles preserved');
}

let created = false;
async function main() {
    assert(!fs.existsSync(base), 'Refusing to overwrite an existing fixture');
    fs.mkdirSync(primary, { recursive: true });
    created = true;
    fs.mkdirSync(sqlDir, { recursive: true });
    fs.writeFileSync(path.join(primary, 'main.c'), 'int primaryStable() { return 1; }\n');
    for (let n = 0; n < 20; n++) {
        fs.writeFileSync(path.join(primary, `overlap${n}.c`),
            Array.from({ length: 400 }, (_, i) => `int overlap_${n}_${i}() { return ${i}; }\n`).join(''));
    }
    for (let n = 0; n < 120; n++) {
        fs.writeFileSync(path.join(sqlDir, `source${n}.c`),
            Array.from({ length: 80 }, (_, i) => `int extra_${n}_${i}() { return ${i}; }\n`).join(''));
    }
    if (acceptanceSequence) {
        initializeAcceptanceRepo(primary);
        initializeAcceptanceRepo(extra);
        const file = path.join(sqlDir, 'source0.c');
        fs.writeFileSync(file, 'int primaryStable();\n' + fs.readFileSync(file, 'utf8')
            .replace('int extra_0_0() { return 0; }', 'int extra_0_0() { return primaryStable(); }'));
    }
    await index(primary);
    let snapshot = primarySnapshot();
    let client = new Client();
    await client.initialize();
    const listed = (await client.request('tools/list')).result.tools;
    const tools = new Map(listed.map(tool => [tool.name, tool]));
    for (const tool of ['workspace_refresh', 'workspace_job_status', 'workspace_job_cancel', 'server_health'])
        assert(tools.has(tool));
    assert.deepEqual(tools.get('workspace_job_status').inputSchema.required, ['job_id']);
    let accepted = client.payload(await client.tool('workspace_add', { path: extra }));
    assert(['queued', 'running'].includes(accepted.status));
    assert(accepted.job_id);
    for (const name of expectStartupBusy ? ['server_health'] :
        ['server_health', 'server_info', 'repo_stats', 'workspace_list']) {
        const started = Date.now();
        const response = await client.tool(name);
        assert(Date.now() - started < 3000, name);
        client.payload(response);
    }
    assert((await client.request('ping')).result);
    assert((await client.request('initialize')).result);
    let result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    assert.equal(result.result.file_count, 120);
    const rootId = result.result.root_id;

    if (acceptanceSequence) {
        await client.close();
        await connectedAcceptance(rootId);
        return;
    }

    if (bulkOnly) {
        await client.close();
        await bulkOverlapReads();
        return;
    }

    // Cold startup must serve the existing extra root immediately, with normal
    // freshness and the real watcher enabled, not just after a tiny job finishes.
    await client.close();
    let release = holdWriter();
    client = new Client('normal', null, true);
    try {
        await client.initialize();
        await waitUntil(() => client.logs.includes('reindex: started'), 'Startup child did not start');
        assert.equal(client.payload(await client.tool('server_health')).status, 'busy');
        if (expectStartupBusy) {
            const response = await client.tool('dir_list', { path: sqlDir, limit: 5 });
            assert.equal(response.error.code, -32603);
            assert.equal(response.error.data.error_code, 'busy');
            assert.equal(response.error.data.retryable, true);
            assert(!client.logs.includes('reindex: done'));
            console.log(`BASELINE: real startup --watch extra-root dir_list denied DURING child: ${response.error.message}`);
            release();
            release = () => {};
            await client.close();
            return;
        }
        const ms = await committedReads(client);
        assert(!client.logs.includes('reindex: done') && !client.logs.includes('reindex: failed'), client.logs);
        console.log(`PASS: normal startup --watch, old extra/primary committed reads DURING child; max ${ms.toFixed(1)}ms`);
    } finally { release(); }
    await waitUntil(() => client.logs.includes('reindex: done'), 'Startup did not commit', 90000);
    assert(client.payload(await client.tool('symbol_search', { query: 'overlap_0_0' })).results.length);
    await client.close();
    client = new Client();
    await client.initialize();

    release = holdWriter();
    const explicitLog = client.logs.length;
    try {
        assert.equal(client.payload(await client.tool('reindex')).status, 'started');
        await waitUntil(() => client.logs.slice(explicitLog).includes('reindex: started'), 'Explicit child did not start');
        const ms = await committedReads(client);
        assert(!client.logs.slice(explicitLog).includes('reindex: done') &&
            !client.logs.slice(explicitLog).includes('reindex: failed'), client.logs);
        const conflict = await client.tool('ingest_traces', { traces: [] });
        assert.equal(conflict.error.code, -32603);
        assert.equal(conflict.error.data.error_code, 'busy');
        assert.equal(conflict.error.data.retryable, true);
        console.log(`PASS: explicit reindex, committed reads DURING child; max ${ms.toFixed(1)}ms; genuine write conflict shape`);
    } finally { release(); }
    await waitUntil(() => client.logs.slice(explicitLog).includes('reindex: done'), 'Explicit child did not commit', 90000);
    snapshot = primarySnapshot();

    fs.writeFileSync(path.join(sqlDir, 'source0.c'), 'int refreshedOnly() { return 7; }\n');
    fs.unlinkSync(path.join(sqlDir, 'source1.c'));
    fs.writeFileSync(path.join(sqlDir, 'added.c'), 'int newlyAdded() { return 8; }\n');
    fs.writeFileSync(path.join(primary, 'main.c'), 'int shouldNotBeReindexed() { return 2; }\n');
    release = holdWriter();
    try {
        accepted = client.payload(await client.tool('workspace_refresh', { path: extra }));
        await waitUntil(() => client.logs.includes(`workspace job: ${accepted.job_id} phase=merging`),
            'Refresh did not reach merge', 90000);
        assert.equal(client.payload(await client.tool('workspace_job_status', { job_id: accepted.job_id })).status, 'running');
        const ms = await committedReads(client);
        assert.equal(client.payload(await client.tool('workspace_job_status', { job_id: accepted.job_id })).status, 'running');
        console.log(`PASS: selected-root refresh, old committed reads DURING merge; max ${ms.toFixed(1)}ms`);
    } finally { release(); }
    result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    const names = symbols(rootId);
    assert(names.has('refreshedOnly') && names.has('newlyAdded'));
    assert(!names.has('extra_0_0') && !names.has('extra_1_0'));
    // Prepared lookup statements and FTS schema were populated before merge.
    const refreshed = client.payload(await client.tool('symbol_search', { query: 'refreshedOnly' }));
    assert(refreshed.results.some(row => row.name === 'refreshedOnly'));
    assert.equal(client.payload(await client.tool('symbol_search', { query: 'extra_0_0' })).results.length, 0);
    const newHandle = refreshed.results.find(row => row.kind === 'function').stable_key;
    assert.equal(client.payload(await client.tool('context_for', { stable_key: newHandle, include_source: false })).symbol.name, 'refreshedOnly');
    assert(symbols().has('primaryStable') && !symbols().has('shouldNotBeReindexed'));
    assert.deepEqual(primarySnapshot(), snapshot);
    assert.equal(client.payload(await client.tool('server_health')).status, 'ready');
    assert((await client.tool('workspace_job_status', { job_id: 'unknown' })).error);
    assert((await client.tool('workspace_refresh', { path: primary })).error);
    accepted = client.payload(await client.tool('workspace_remove', { path: extra }));
    result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    assert.deepEqual(client.payload(await client.tool('workspace_list')).roots, []);
    let logs = await client.close();
    assert(logs.includes('stdin EOF') && logs.includes('workspace job:') && logs.includes('[child]'));
    assert(logs.includes('Changed: 1') && logs.includes('Deleted: 1'), logs.slice(-4000));
    console.log('PASS: schemas, concurrent health/initialize/ping, selected-root refresh, removal, EOF');

    client = new Client();
    await client.initialize();
    for (let round = 0; round < 3; ++round) {
        // Populate several prepared-statement cache entries before explicit reindex.
        for (const [name, args] of [
            ['file_search', { pattern: '*.c' }],
            ['symbol_search', { query: '*' }],
            ['symbol_list', {}],
            ['dir_list', { path: '.' }],
            ['workspace_list', {}]
        ]) client.payload(await client.tool(name, args));
        for (let n = 0; n < 20; ++n) {
            fs.writeFileSync(path.join(primary, `race${n}.c`),
                Array.from({ length: 400 }, (_, i) =>
                    `int race_${round}_${n}_${i}() { return ${round + i}; }\n`).join(''));
        }
        const logStart = client.logs.length;
        assert.equal(client.payload(await client.tool('reindex')).status, 'started');
        const startedDeadline = Date.now() + 3000;
        while (!client.logs.slice(logStart).includes('reindex: started') && Date.now() < startedDeadline)
            await sleep(1);
        assert(client.logs.slice(logStart).includes('reindex: started'), client.logs.slice(-2000));
        assert(!client.logs.slice(logStart).includes('reindex: done'), 'Expected an active primary writer');
        accepted = client.payload(await client.tool('workspace_remove', { path: extra }));
        let queued = client.payload(await client.tool('workspace_job_status', { job_id: accepted.job_id }));
        assert.equal(queued.phase, 'waiting_for_writer', JSON.stringify(queued));
        // Pipeline admission-free workspace requests across index completion. They must
        // never reset/finalize the graph cache while its owner transitions admission.
        const completionDeadline = Date.now() + 90000;
        do {
            const traffic = [];
            for (let n = 0; n < 100; ++n) {
                traffic.push(client.tool('workspace_refresh', { path: extra }));
                traffic.push(client.tool('server_health'));
                traffic.push(client.tool('symbol_search', { query: '*' }));
            }
            for (const response of await Promise.all(traffic)) {
                if (response.error) {
                    assert.equal(response.error.data.error_code, 'invalid_input');
                } else {
                    const value = client.payload(response);
                    if (value.job_id) {
                        assert.equal(value.operation, 'refresh');
                        accepted = value;
                    }
                }
            }
            assert(Date.now() < completionDeadline, 'Explicit reindex exceeded 90 seconds');
        } while (!client.logs.slice(logStart).includes('reindex: done'));
        result = await client.waitJob(accepted.job_id);
        assert(['completed', 'failed'].includes(result.status), JSON.stringify(result));
        if (result.status === 'failed') assert(result.error.includes('existing extra root'), result.error);
        assert(client.logs.slice(logStart).includes('reindex: done'), client.logs.slice(-2000));
        assert.equal(client.payload(await client.tool('server_health')).status, 'ready');
        const fresh = client.payload(await client.tool('symbol_search', { query: `race_${round}_0_0` }));
        assert(JSON.stringify(fresh).includes(`race_${round}_0_0`), JSON.stringify(fresh));
    }
    await client.close();
    assert(symbols().has('shouldNotBeReindexed'));
    console.log('PASS: explicit reindex completion with queued workspace jobs, pipelined cache traffic, fresh queries');

    client = new Client();
    await client.initialize();
    accepted = client.payload(await client.tool('workspace_add', { path: extra }));
    const cancelled = client.payload(await client.tool('workspace_job_cancel', { job_id: accepted.job_id }));
    assert(cancelled.cancel_requested || cancelled.status === 'completed');
    result = await client.waitJob(accepted.job_id);
    assert(['cancelled', 'completed'].includes(result.status), JSON.stringify(result));
    await client.close();
    console.log('PASS: explicit cancellation and owned worker shutdown');

    client = new Client();
    await client.initialize();
    accepted = client.payload(await client.tool('workspace_add', { path: extra }));
    logs = await client.close();
    assert(logs.includes('stdin EOF') && logs.includes('draining workspace job'));
    assert(!fs.existsSync(primaryDb + '.lock'));
    assert(!fs.existsSync(path.join(extra, '.codetopo', 'index.sqlite.lock')));
    console.log('PASS: EOF during workspace operation, cleanup of writer locks');

    client = new Client('off', 1);
    await client.initialize();
    client.child.stdin.write('{"jsonrpc":'); // Idle enforcement also covers incomplete NDJSON.
    await sleep(1500);
    logs = await client.close(5000);
    assert(logs.includes('idle timeout'));
    console.log('PASS: opt-in idle timeout without getline hang');

    client = new Client('normal');
    await client.initialize();
    client.payload(await client.tool('server_health'));
    logs = await client.close();
    assert(logs.includes('draining index child'));
    console.log('PASS: startup background indexing and EOF lifetime');

    client = new Client('lazy', null, true);
    await client.initialize();
    assert.equal(client.payload(await client.tool('server_health')).status, 'ready');
    accepted = client.payload(await client.tool('workspace_add', { path: extra }));
    result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    accepted = client.payload(await client.tool('workspace_refresh', { path: extra }));
    result = await client.waitJob(accepted.job_id);
    assert.equal(result.status, 'completed', JSON.stringify(result));
    fs.writeFileSync(path.join(primary, 'main.c'), 'int watchedChange() { return 3; }\n');
    await sleep(200);
    logs = await client.close();
    assert(logs.includes('watcher: stopped') && logs.includes('stdin EOF'), logs);
    console.log('PASS: embedded watcher shutdown and callback lifetime');

    // A non-WAL legacy DB with a real exclusive writer cannot provide a snapshot.
    // Preserve an honest retryable error, bound its wait, then recover on release.
    const locked = new DatabaseSync(primaryDb);
    locked.exec('PRAGMA journal_mode=DELETE');
    client = new Client();
    try {
        await client.initialize();
        locked.exec('BEGIN EXCLUSIVE');
        const start = performance.now();
        const unavailable = await client.tool('dir_list', { path: '.' });
        const elapsed = performance.now() - start;
        assert.equal(unavailable.error.code, -32603);
        assert.equal(unavailable.error.data.error_code, 'busy');
        assert.equal(unavailable.error.data.retryable, true);
        assert(elapsed < 1500, `Genuine read lock waited ${elapsed}ms`);
        locked.exec('ROLLBACK');
        assert(client.payload(await client.tool('dir_list', { path: '.' })).files.length > 0);
        await client.close();
        console.log(`PASS: genuine SQLite read lock error shape, ${elapsed.toFixed(1)}ms bounded wait and recovery`);
    } finally {
        if (locked.isTransaction) locked.exec('ROLLBACK');
        locked.close();
    }
    withDb(db => db.exec('PRAGMA journal_mode=WAL'));

    withDb(db => db.prepare("UPDATE kv SET value='999' WHERE key='schema_version'").run());
    const bad = spawn(exe, ['mcp', '--root', primary, '--db', primaryDb],
        { stdio: ['ignore', 'pipe', 'pipe'] });
    let failureLog = '';
    bad.stdout.resume();
    bad.stderr.on('data', data => { failureLog += data.toString(); });
    const timer = setTimeout(() => bad.kill(), 5000);
    try {
        const code = await new Promise((resolve, reject) => {
            bad.once('close', resolve);
            bad.once('error', reject);
        });
        assert.equal(code, 3);
        assert(failureLog.includes('shutdown: startup_error'), failureLog);
    } finally {
        clearTimeout(timer);
    }
    console.log('PASS: explicit startup-error shutdown diagnostic');

    if (!expectStartupBusy) {
        // Keep bulk stress independent of the legacy fixture and its snapshots.
        const bulk = spawn(process.execPath, [__filename, exe, '--bulk-only'], { stdio: ['ignore', 'inherit', 'inherit'] });
        const code = await new Promise((resolve, reject) => {
            bulk.once('close', resolve);
            bulk.once('error', reject);
        });
        assert.equal(code, 0, 'Isolated production bulk suite failed');
    }
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
}).finally(async () => {
    for (const client of clients) {
        if (client.child.exitCode === null && client.child.signalCode === null) {
            // Drain owned children before deleting their source/DB fixtures, including
            // assertion failures during an active writer. Never kill index children.
            await client.close();
        }
        client.lines.close();
    }
    if (created) fs.rmSync(base, { recursive: true, force: true });
});
