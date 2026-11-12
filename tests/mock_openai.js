/*
 * Minimal OpenAI-compatible mock server for Agent98 integration tests.
 *
 * Usage: node tests/mock_openai.js [port]
 *
 * Behaviour:
 *   - "list files" in the latest user message -> one list_dir tool call
 *   - anything else -> a short text answer
 *   - honours "stream": true with SSE chunks
 */

const http = require('http');

const port = parseInt(process.argv[2] || '8787', 10);

function sse(res, obj) {
  res.write('data: ' + JSON.stringify(obj) + '\n\n');
}

function chunk(delta, finish) {
  return {
    id: 'chatcmpl-mock',
    object: 'chat.completion.chunk',
    created: Math.floor(Date.now() / 1000),
    model: 'mock-model',
    choices: [{ index: 0, delta: delta, finish_reason: finish || null }]
  };
}

/* Counters for the retry tests: the first request fails on purpose, the
 * second one succeeds. */
const flakyCounters = {};

const server = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    if (req.method === 'GET' && req.url.indexOf('/models') >= 0) {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        object: 'list',
        data: [{ id: 'mock-model', object: 'model', owned_by: 'mock' }]
      }));
      return;
    }
    if (req.method !== 'POST' || req.url.indexOf('/chat/completions') < 0) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('not found');
      return;
    }
    let payload = {};
    try { payload = JSON.parse(body); } catch (e) { payload = {}; }

    const messages = payload.messages || [];
    const users = messages.filter((m) => m.role === 'user');
    const lastUser = users.length ? String(users[users.length - 1].content || '') : '';
    const last = messages.length ? messages[messages.length - 1] : {};
    const freshUser = last.role === 'user';
    const toolMessages = messages.filter((m) => m.role === 'tool');
    const lastTool = toolMessages.length
      ? String(toolMessages[toolMessages.length - 1].content || '')
      : '';
    const hasToolResult = toolMessages.length > 0;
    const wantsTool = /list files/i.test(lastUser) && freshUser;
    const wantsWrite = /write file/i.test(lastUser) && freshUser;
    const stream = !!payload.stream;
    const wantsRetry503 = /retry 503/i.test(lastUser) && freshUser;
    const wantsRetryDrop = /retry drop/i.test(lastUser) && freshUser;
    let retried = false;

    if (wantsRetry503 || wantsRetryDrop) {
      const key = wantsRetry503 ? '503' : 'drop';
      flakyCounters[key] = (flakyCounters[key] || 0) + 1;
      if (flakyCounters[key] === 1) {
        if (wantsRetry503) {
          res.writeHead(503, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({
            error: { message: 'mock service overloaded', type: 'server_error' }
          }));
        } else {
          req.socket.destroy();
        }
        return;
      }
      retried = true;
    }

    if (wantsTool || wantsWrite) {
      const call = wantsWrite ? {
        id: 'call_mock_write',
        type: 'function',
        function: {
          name: 'write_file',
          arguments: JSON.stringify({ path: 'mock.txt', content: 'written by mock\n' })
        }
      } : {
        id: 'call_mock_1',
        type: 'function',
        function: { name: 'list_dir', arguments: '{"path":"."}' }
      };
      if (stream) {
        res.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache',
          'Connection': 'close'
        });
        sse(res, chunk({ role: 'assistant' }));
        sse(res, chunk({ tool_calls: [{
          index: 0,
          id: call.id,
          type: 'function',
          function: call.function
        }] }));
        sse(res, chunk({}, 'tool_calls'));
        res.write('data: [DONE]\n\n');
        res.end();
      } else {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
          id: 'chatcmpl-mock',
          object: 'chat.completion',
          created: Math.floor(Date.now() / 1000),
          model: 'mock-model',
          choices: [{
            index: 0,
            message: { role: 'assistant', content: null, tool_calls: [call] },
            finish_reason: 'tool_calls'
          }]
        }));
      }
      return;
    }

    let text = 'Hello from the mock server.';
    if (retried) {
      text = 'The retry succeeded.';
    } else if (hasToolResult) {
      if (/Wrote \d+ bytes/.test(lastTool)) {
        text = 'I wrote the file.';
      } else if (/denied by the user/i.test(lastTool)) {
        text = 'The write was denied.';
      } else {
        text = 'I listed the workspace files for you.';
      }
    }

    if (stream) {
      res.writeHead(200, {
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache',
        'Connection': 'close'
      });
      sse(res, chunk({ role: 'assistant' }));
      const words = text.split(' ');
      for (let i = 0; i < words.length; i++) {
        const piece = (i ? ' ' : '') + words[i];
        sse(res, chunk({ content: piece }));
      }
      sse(res, chunk({}, 'stop'));
      res.write('data: [DONE]\n\n');
      res.end();
    } else {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        id: 'chatcmpl-mock',
        object: 'chat.completion',
        created: Math.floor(Date.now() / 1000),
        model: 'mock-model',
        choices: [{
          index: 0,
          message: { role: 'assistant', content: text },
          finish_reason: 'stop'
        }],
        usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 }
      }));
    }
  });
});

server.listen(port, '127.0.0.1', () => {
  console.log('mock OpenAI server listening on http://127.0.0.1:' + port + '/v1');
});
