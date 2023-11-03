/*
 * Websocket for server communication.
 */
let socket = null;

const connection = navigator.connection || navigator.mozConnection || null;
if (connection === null) {
  console.error('Network Information API not supported.');
}

/*
 * Connects with websocket port
 */
function connect() {
  console.info(`Location hostname: ${location.hostname}`);

  socket = new WebSocket(`ws://${location.hostname}:10024`, 'broadcast');
  socket.binaryType = 'arraybuffer';
  socket.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg !== null && typeof msg === 'object') {
      self.postMessage({ cmd: 'data', data: msg });
    }
  };

  socket.onclose = () => {
    console.warn('Connection closed.');
    socket = null;
    self.postMessage({ cmd: 'disconnected', data: null });
    setTimeout(() => {
      connect();
    }, 10000);
  };

  /*
   * Event functions for socket
   */
  socket.onopen = () => {
    console.info('Connected with websocket port');
    self.postMessage({ cmd: 'connected', data: null });
  };
}

self.onmessage = (e) => {
  const msg = e.data;
  switch (msg.cmd) {
    case 'connect':
      connect();
      break;
    default:
      console.error(`Unknown command: ${msg.cmd}`);
  }
};
