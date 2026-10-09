'use strict';
// window.efml is the same bridge Better-Endfield pages know as window.betterEndfield.
const host = window.efml || window.betterEndfieldNext || window.betterEndfield;
const log = document.querySelector('#log');
const connection = document.querySelector('#connection');
let configuration = {};

function write(value) {
  log.textContent = (typeof value === 'string' ? value : JSON.stringify(value, null, 2)) + '\n\n' + log.textContent;
}

async function status() {
  try {
    const value = await host.status();
    connection.textContent = value.connected ? '游戏已连接' : '游戏未连接';
    write(value);
  } catch (error) {
    write(error.message);
  }
}

host.onmessage(message => {
  if (message.kind === 'reply' && message.body?.counter !== undefined) document.querySelector('#count').value = message.body.counter;
  if (message.kind === 'event' && message.body?.type === 'counter') document.querySelector('#count').value = message.body.value;
  write(message);
});

document.querySelector('#save').onclick = async () => {
  try {
    configuration = { ...configuration, label: document.querySelector('#label').value };
    write(await host.saveConfig(configuration));
  } catch (error) {
    write(error.message);
  }
};

document.querySelector('#send').onclick = async () => {
  try {
    write(await host.send(JSON.parse(document.querySelector('#body').value)));
  } catch (error) {
    write(error.message);
  }
};

document.querySelector('#refresh').onclick = status;

(async () => {
  try {
    configuration = await host.readConfig();
    document.querySelector('#label').value = configuration.label ?? 'Echo';
    await status();
  } catch (error) {
    write(error.message);
  }
})();
