// Production dispatcher regressions. Node 22+; fixture stays inside the repo.
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const assert = require('node:assert/strict');
const { DatabaseSync } = require('node:sqlite');

const exe = path.resolve(process.argv[2]);
const base = path.join(process.cwd(), '.codetopo-lookup-stdio-test');
const primary = path.join(base, 'primary');
const extra = path.join(primary, '.lookup-extra');
const dbPath = path.join(primary, '.codetopo', 'index.sqlite');
let child, exited, created = false;
let sequence = 0;
const pending = new Map();
let logs = '';
const notifications = [];

async function run(args) {
    const process = spawn(exe, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    process.stdout.on('data', data => { output += data; });
    process.stderr.on('data', data => { output += data; });
    const code = await new Promise((resolve, reject) => {
        process.once('close', resolve);
        process.once('error', reject);
    });
    assert.equal(code, 0, output);
}

async function request(method, params) {
    const id = ++sequence;
    let timer;
    const response = new Promise((resolve, reject) => {
        timer = setTimeout(() => {
            pending.delete(id);
            reject(new Error(`${method} timed out\n${logs.slice(-2000)}`));
        }, 30000);
        pending.set(id, { resolve, reject });
    });
    child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    try { return await response; } finally { clearTimeout(timer); }
}

const call = (name, arguments) => request('tools/call', { name, arguments });
function payload(response) {
    assert(!response.error, JSON.stringify(response));
    const result = JSON.parse(response.result.content[0].text);
    assert(!result.error, JSON.stringify(result));
    return result;
}
const tool = async (name, args) => payload(await call(name, args));
function errorCode(response) {
    const error = response.error || JSON.parse(response.result.content[0].text).error;
    assert(error, JSON.stringify(response));
    return error.data.error_code;
}
function variants(file) {
    return [...new Set([file, file.replaceAll('\\', '/'),
        ...(process.platform === 'win32' ? [file.replaceAll('/', '\\'),
            file.replaceAll('/', '\\').replace('\\', '/')] : [])])];
}

async function main() {
    assert(!fs.existsSync(base), 'Refusing to overwrite existing fixture');
    fs.mkdirSync(path.join(primary, 'src'), { recursive: true });
    fs.mkdirSync(path.join(primary, 'src', 'deep'), { recursive: true });
    created = true;
    fs.writeFileSync(path.join(primary, 'src', 'engine.cpp'),
        'int target() { return 1; }\nint caller() { return target(); }\n'
        + 'int overloaded(int x) { return x; }\nint overloaded(double x) { return int(x); }\n');
    fs.writeFileSync(path.join(primary, 'top.cpp'), 'int top() { return 3; }\n');
    fs.writeFileSync(path.join(primary, 'src', 'deep', 'nested.cpp'), 'int nested() { return 4; }\n');
    await run(['index', '--root', primary, '--threads', '1', '--arena-size', '8']);
    fs.mkdirSync(path.join(extra, 'src'), { recursive: true });
    fs.writeFileSync(path.join(extra, 'src', 'engine.cpp'),
        'int target() { return 2; }\nint extraCaller() { return target(); }\n');
    await run(['index', '--root', extra, '--threads', '1', '--arena-size', '8']);
    for (const root of [primary, extra]) {
        const db = new DatabaseSync(path.join(root, '.codetopo', 'index.sqlite'));
        try {
            db.function('codetopo_camel_split', { deterministic: true }, value => value);
            // Use a controlled graph so unrelated extractor duplicate-symbol
            // behavior cannot turn the unique-selector fixture into an overload.
            const file = db.prepare('SELECT id FROM files WHERE path=?').get('src/engine.cpp').id;
            db.prepare('DELETE FROM nodes WHERE file_id=?').run(file);
            const insert = db.prepare("INSERT INTO nodes(node_type,file_id,kind,name,qualname,start_line,end_line,stable_key) VALUES('symbol',?,'function',?,?,?,?,?)");
            const target = insert.run(file, 'target', 'ns::target', 1, 1, 'src/engine.cpp::function::target').lastInsertRowid;
            const caller = insert.run(file, 'caller', 'caller', 2, 2, 'src/engine.cpp::function::caller').lastInsertRowid;
            db.prepare("INSERT INTO edges(src_id,dst_id,kind,confidence) VALUES(?,?,'calls',1)").run(caller, target);
            db.prepare("INSERT INTO refs(file_id,kind,name,start_line,start_col,end_line,end_col,containing_node_id) VALUES(?,'http_call',?,1,1,1,2,?)")
                .run(file, root === primary ? '/primary' : '/extra', target);
            if (root === primary) {
                insert.run(file, 'overloaded', 'overloaded', 3, 3, 'src/engine.cpp::function::overloaded');
                insert.run(file, 'overloaded', 'overloaded', 4, 4, 'src/engine.cpp::function::overloaded#2');
            }
        } finally { db.close(); }
    }
    child = spawn(exe, ['mcp', '--root', primary, '--db', dbPath, '--freshness', 'off'],
        { stdio: ['pipe', 'pipe', 'pipe'] });
    exited = new Promise((resolve, reject) => {
        child.once('close', resolve);
        child.once('error', reject);
    });
    child.stderr.on('data', data => { logs += data; });
    readline.createInterface({ input: child.stdout }).on('line', line => {
        try {
            const message = JSON.parse(line);
            if (message.id == null) {
                assert.equal(message.method, 'notifications/message', line);
                assert.equal(message.params.level, 'info', line);
                assert.equal(typeof message.params.data, 'string', line);
                notifications.push(message);
                return;
            }
            const waiter = pending.get(message.id);
            assert(waiter, line);
            pending.delete(message.id);
            waiter.resolve(message);
        } catch (error) {
            for (const waiter of pending.values()) waiter.reject(error);
            pending.clear();
        }
    });
    const initialized = await request('initialize', {});
    assert(initialized.result.capabilities.logging);
    assert.equal(notifications.length, 0, 'No logs before client initialization');
    child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    assert.equal((await tool('server_health', {})).status, 'ready');
    await tool('repo_stats', {});
    assert(notifications.some(message => message.params.data.includes('live logs enabled')));
    assert(notifications.some(message => message.params.data.includes('tool: repo_stats')));
    assert(notifications.some(message => message.params.data.includes('done: repo_stats')));
    assert.deepEqual((await request('logging/setLevel', { level: 'warning' })).result, {});
    const beforeFilteredCall = notifications.length;
    await tool('repo_stats', {});
    assert.equal(notifications.length, beforeFilteredCall, 'Info logs must respect setLevel');
    assert.deepEqual((await request('logging/setLevel', { level: 'info' })).result, {});
    const invalidLevel = await request('logging/setLevel', { level: 'invalid' });
    assert.equal(invalidLevel.error.code, -32602);
    await tool('repo_stats', {});
    assert(notifications.length > beforeFilteredCall, 'Info logging resumes after setLevel');
    const schemas = (await request('tools/list', {})).result.tools;
    const nodeTools = ['context_for', 'symbol_get', 'references', 'method_fields',
        'find_similar', 'callers_approx', 'callees_approx', 'impact_of'];
    for (const name of nodeTools) {
        const schema = schemas.find(item => item.name === name).inputSchema;
        assert(schema.anyOf, `${name}: ${JSON.stringify(schema)}`);
        assert.deepEqual(schema.anyOf.map(item => item.required),
            [['stable_key'], ['node_id'], ['symbol', 'file']]);
        assert.equal(schema.properties.symbol.minLength, 1);
        assert.equal(schema.properties.file.type, 'string');
    }
    const job = await tool('workspace_add', { path: extra });
    assert(job.job_id);
    const deadline = Date.now() + 90000;
    let status;
    do {
        assert(Date.now() < deadline, 'Workspace merge timed out');
        await new Promise(resolve => setTimeout(resolve, 25));
        status = await tool('workspace_job_status', { job_id: job.job_id });
    } while (!['completed', 'failed', 'cancelled'].includes(status.status));
    assert.equal(status.status, 'completed', JSON.stringify(status));

    // Reproduce forward-stored nested roots, not just the merge's native spelling.
    const db = new DatabaseSync(dbPath);
    try {
        const registered = db.prepare('SELECT id, path FROM roots WHERE path=?').get(extra);
        assert(registered);
        const forward = extra.replaceAll('\\', '/');
        const mergedFiles = db.prepare('SELECT id, path FROM files WHERE root_id=?').all(registered.id);
        assert.equal(mergedFiles.length, 1);
        for (const file of mergedFiles)
            db.prepare('UPDATE files SET path=? WHERE id=?').run(file.path.replaceAll('\\', '/'), file.id);
        db.prepare('UPDATE roots SET path=? WHERE id=?').run(forward, registered.id);
    } finally { db.close(); }

    const files = await tool('file_search', { pattern: '*engine.cpp' });
    assert.equal(files.total, 2);
    for (const row of files.results) {
        const found = await tool('symbol_search', { query: 'target', file_pattern: row.path });
        const targets = found.results.filter(item => item.name === 'target' && item.kind === 'function');
        assert.equal(targets.length, 1, JSON.stringify(found));
        const symbol = targets[0];
        const options = { max_source_lines: 1, max_callers: 1, max_callees: 1,
            include_candidates: false };
        const handle = { stable_key: symbol.stable_key, ...options };
        const baseline = await tool('context_for', handle);
        assert(baseline.symbol.source.includes('int target()'));
        assert.equal(baseline.callers.length, 1);
        assert.deepEqual(await tool('context_for', { node_id: symbol.node_id, ...options }), baseline);
        const absolute = path.isAbsolute(row.path) ? row.path : path.join(primary, row.path);
        for (const file of [...new Set([...variants(row.path), ...variants(absolute),
            ...(!path.isAbsolute(row.path) ? variants('./' + row.path) : [])])]) {
            assert.deepEqual(await tool('context_for', { symbol: 'target', file, ...options }), baseline);
            assert.deepEqual(await tool('context_for', { symbol: 'ns::target', file, ...options }), baseline);
            assert.deepEqual(await tool('context_for', { symbol: 'target', file,
                include_source: false, ...options }),
                await tool('context_for', { ...handle, include_source: false }));
            for (const name of nodeTools.filter(name => name !== 'context_for')) {
                const selected = await call(name, { symbol: 'target', file, include_source: false });
                const byHandle = await call(name, { stable_key: symbol.stable_key, include_source: false });
                delete selected.id;
                delete byHandle.id;
                assert.deepEqual(selected, byHandle);
            }
            const summary = await tool('file_summary', { path: file });
            assert(summary.symbols || summary.results);
            assert.deepEqual(await tool('file_summary', { file_path: file }), summary);
            assert.equal((await tool('file_overview', { path: file })).file, row.path);
            assert((await tool('source_at', { path: file, start_line: 1, end_line: 1 })).source.includes('int target()'));
            assert.equal((await tool('symbol_list', { file_path: file })).total, summary.total);
            assert.equal((await tool('symbols_in_path', { path: file })).total, summary.total);
            assert.equal((await tool('symbol_search', { query: '*', file_pattern: file })).total, summary.total);
            assert.equal((await tool('context_by_name', { name: 'target', file_pattern: file })).symbol.node_id, symbol.node_id);
            assert.equal((await tool('dependency_cluster', { path: file })).clusters.length, 0);
            await tool('file_deps', { path: file });
            assert.equal((await tool('file_search', { pattern: file })).results[0].path, row.path);
        }
        const parent = row.path.slice(0, row.path.lastIndexOf('/'));
        for (const directory of variants(parent)) {
            assert.equal((await tool('dir_list', { path: directory })).files.length, 1);
            const subtreeFiles = await tool('file_search', { pattern: directory + '/*' });
            const subtreeCounts = await Promise.all(subtreeFiles.results.map(item =>
                tool('symbol_list', { file_path: item.path })));
            assert.equal((await tool('symbols_in_path', { path: directory })).total,
                subtreeCounts.reduce((total, item) => total + item.total, 0));
            assert((await tool('dir_tree', { path: directory })).tree);
            assert.equal((await tool('symbol_search', { query: 'target', file_pattern: directory + '/*' })).total, 1);
        }
        assert.deepEqual(await tool('context_for', { ...handle, node_id: -1, symbol: 0, file: [] }), baseline);
        assert.equal(errorCode(await call('context_for', { node_id: symbol.node_id,
            expected_stable_key: 'wrong' })), 'invalid_input');
    }
    const ambiguous = await tool('context_for', { symbol: 'overloaded', file: 'src/engine.cpp' });
    assert.equal(ambiguous.ambiguous, true);
    assert.equal(ambiguous.candidates.length, 2);
    for (const candidate of ambiguous.candidates)
        assert.equal((await tool('context_for', { stable_key: candidate.stable_key })).symbol.node_id, candidate.node_id);
    assert.equal((await tool('context_by_name', { name: 'target' })).ambiguous, true);
    for (const args of [{ symbol: 'target', file: 'engine.cpp' },
        { symbol: 'missing', file: 'src/engine.cpp' }, { symbol: 'target', file: 'missing.cpp' }])
        assert.equal(errorCode(await call('context_for', args)), 'not_found');
    for (const args of [{}, { symbol: 'target' }, { symbol: false, file: 'src/engine.cpp' },
        { symbol: 'target', file: ['src/engine.cpp'] }, { symbol: '', file: 'src/engine.cpp' },
        { symbol: 'target', file: '../primary/src/engine.cpp' }, { symbol: 'target', file: 'src\0/engine.cpp' },
        { node_id: 1.5 }, { node_id: -1 }, { stable_key: [] }])
        assert.equal(errorCode(await call('context_for', args)), 'invalid_input');
    assert.equal((await tool('symbol_list', { file_path: 'missing.cpp' })).total, 0);
    assert.equal((await tool('symbol_list', { file_path: 'engine.cpp' })).total, 0);
    assert.equal(errorCode(await call('symbol_list', { file_path: 5 })), 'invalid_input');
    assert.equal(errorCode(await call('symbol_search', { query: '*', file_pattern: [] })), 'invalid_input');
    assert.equal(errorCode(await call('source_at', { path: false, start_line: 1, end_line: 1 })), 'invalid_input');
    const rootListing = await tool('dir_list', { path: '.' });
    delete rootListing.directory;
    for (const directory of ['.', './', '']) {
        const listing = await tool('dir_list', { path: directory });
        delete listing.directory;
        assert.deepEqual(listing, rootListing);
    }
    for (const directory of ['.', './'])
        assert.equal((await tool('symbols_in_path', { path: directory })).total,
            (await tool('symbols_in_path', { path: '.' })).total);
    const primaryFiles = ['top.cpp', 'src/engine.cpp', 'src/deep/nested.cpp'];
    const primarySymbols = (await Promise.all(primaryFiles.map(async file =>
        (await tool('symbol_list', { file_path: file, include_handles: true })).results
            .map(item => ({ ...item, file }))))).flat();
    const ids = result => result.results.map(item => item.node_id).sort((a, b) => a - b);
    const primaryIds = primarySymbols.map(item => item.node_id).sort((a, b) => a - b);
    for (const directory of variants(primary)) {
        assert.deepEqual(ids(await tool('symbols_in_path', { path: directory, include_handles: true })), primaryIds);
        assert.equal((await tool('symbols_in_path', { path: directory, recursive: false })).total,
            (await tool('symbol_list', { file_path: 'top.cpp' })).total);
        assert.equal((await tool('dir_tree', { path: directory })).file_count, 3);
        const listing = await tool('dir_list', { path: directory });
        assert.deepEqual(listing.files.map(item => item.path), ['top.cpp']);
        assert.deepEqual(listing.subdirectories, ['src/']);
        for (const suffix of ['engine.cpp', 'n*.cpp']) {
            for (const pattern of variants(path.join(directory, suffix)))
                assert.equal((await tool('file_search', { pattern })).total, 0);
        }
        for (const pattern of variants(path.join(directory, 't*.cpp')))
            assert.deepEqual((await tool('file_search', { pattern })).results.map(item => item.path), ['top.cpp']);
    }
    assert((await tool('symbols_in_path', { path: '.' })).total > primarySymbols.length);
    assert.equal((await tool('dir_tree', { path: '.' })).file_count, 4);
    for (const suffix of ['*', '**', 'src/*', 'src/**']) {
        const expectedFiles = primaryFiles.filter(file => !suffix.startsWith('src/') || file.startsWith('src/')).sort();
        const expectedIds = primarySymbols.filter(item => expectedFiles.includes(item.file)).map(item => item.node_id).sort((a, b) => a - b);
        for (const pattern of variants(path.join(primary, suffix))) {
            assert.deepEqual((await tool('file_search', { pattern })).results.map(item => item.path).sort(), expectedFiles);
            assert.deepEqual(ids(await tool('symbol_search', { query: '*', file_pattern: pattern })), expectedIds);
            assert.equal((await tool('symbol_search', { query: 'target', file_pattern: pattern })).total, 1);
            const selected = await tool('context_by_name', { name: 'target', file_pattern: pattern });
            assert.equal(selected.symbol.file_path, 'src/engine.cpp');
            const http = await tool('list_http_calls', { file_pattern: pattern });
            assert.equal(http.total, 1);
            assert.equal(http.results[0].file, 'src/engine.cpp');
            const search = await tool('code_search', { query: 'target', file_pattern: pattern });
            assert.equal(search.total_files, 1);
            const entries = await tool('entrypoints', { scope: pattern });
            assert(entries.results.length > 0);
            assert(entries.results.every(item => expectedFiles.includes(item.file_path)));
        }
    }
    assert.equal((await tool('file_search', { pattern: '*' })).total, 4);
    const extraFile = extra.replaceAll('\\', '/') + '/src/engine.cpp';
    const extraIds = ids(await tool('symbol_search', { query: '*', file_pattern: extraFile }));
    assert(extraIds.length > 0);
    for (const suffix of ['*', '**', 'src/*', 'src/**']) {
        for (const pattern of variants(path.join(extra, suffix))) {
            assert.deepEqual((await tool('file_search', { pattern })).results.map(item => item.path), [extraFile]);
            assert.deepEqual(ids(await tool('symbol_search', { query: '*', file_pattern: pattern })), extraIds);
            assert.equal((await tool('symbol_search', { query: 'target', file_pattern: pattern })).total, 1);
            assert.equal((await tool('context_by_name', { name: 'target', file_pattern: pattern })).symbol.file_path, extraFile);
        }
    }
    for (const pattern of variants(extra + '-other/*')) {
        assert.equal((await tool('file_search', { pattern })).total, 0);
        assert.equal((await tool('symbol_search', { query: '*', file_pattern: pattern })).total, 0);
    }
    console.log('PASS: production schemas, scoped primary/nested forward-stored extra roots, native/forward/mixed root wildcards, bounded glob semantics, shared selector parity, ambiguity and invalid inputs');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    if (child && child.exitCode === null) child.stdin.end();
    if (exited) {
        const code = await exited;
        if (code !== 0) { console.error(logs); process.exitCode = 1; }
    }
    if (created) fs.rmSync(base, { recursive: true, force: true });
});
