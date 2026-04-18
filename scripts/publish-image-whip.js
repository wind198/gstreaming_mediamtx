#!/usr/bin/env node

const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const IMAGE_PATH = path.resolve(process.env.IMAGE_PATH || 'test.jpeg');
const STREAM_NAME = process.env.STREAM_NAME || 'test';
const FPS = process.env.FPS || '30';
const MEDIAMTX_WHIP_BASE = process.env.MEDIAMTX_WHIP_BASE || 'http://127.0.0.1:8889';

if (!fs.existsSync(IMAGE_PATH)) {
  console.error(`Image not found at: ${IMAGE_PATH}`);
  process.exit(1);
}

const whipEndpoint = `${MEDIAMTX_WHIP_BASE.replace(/\/$/, '')}/${STREAM_NAME}/whip`;

const inspect = spawnSync('gst-inspect-1.0', ['whipclientsink'], { encoding: 'utf8' });
if (inspect.status !== 0) {
  console.error('GStreamer element "whipclientsink" is not available.');
  console.error('Install gst-plugins-rs (rswebrtc) so WHIP publishing can work.');
  process.exit(1);
}

const pipelineArgs = [
  '-e',
  'whipclientsink', 'name=whip', `signaller::whip-endpoint=${whipEndpoint}`,
  'filesrc', `location=${IMAGE_PATH}`,
  '!', 'jpegdec',
  '!', 'imagefreeze',
  '!', 'videoconvert',
  '!', 'videorate',
  '!', `video/x-raw,format=I420,framerate=${FPS}/1`,
  '!', 'queue',
  '!', 'whip.',
];

console.log('Starting WHIP publisher with pipeline:');
console.log(`gst-launch-1.0 ${pipelineArgs.join(' ')}`);

const gst = spawn('gst-launch-1.0', pipelineArgs, {
  stdio: 'inherit',
});

gst.on('error', (err) => {
  console.error('Failed to start gst-launch-1.0. Is GStreamer installed?', err.message);
  process.exit(1);
});

gst.on('exit', (code, signal) => {
  if (signal) {
    console.log(`GStreamer stopped by signal: ${signal}`);
    process.exit(0);
  }

  process.exit(code ?? 0);
});

const shutdown = () => {
  if (!gst.killed) {
    gst.kill('SIGINT');
  }
};

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
