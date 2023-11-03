/*
 * Create worker thread for server communication.
 */
const serverCommunicationWorker = new Worker('./js/ws.worker.js');

/*
 * Update user interface
 */
function UpdateGui(upsStatus) {
  document.getElementById('fieldInputVoltage').value = upsStatus.inputVoltage.toFixed(1);
  document.getElementById('fieldInputCurrent').value = (upsStatus.inputCurrent / 1000).toFixed(3);
  document.getElementById('fieldOutputVoltage').value = upsStatus.outputVoltage.toFixed(1);
  document.getElementById('fieldBatteryVoltage').value = upsStatus.batteryVoltage.toFixed(1);
  document.getElementById('fieldBatteryCurrent').value = (upsStatus.batteryCurrent / 1000).toFixed(3);
  document.getElementById('fieldCell1Voltage').value = upsStatus.vcap1Voltage.toFixed(1);
  document.getElementById('fieldCell2Voltage').value = upsStatus.vcap2Voltage.toFixed(1);
  document.getElementById('fieldCell3Voltage').value = upsStatus.vcap3Voltage.toFixed(1);
  document.getElementById('fieldCell4Voltage').value = upsStatus.vcap4Voltage.toFixed(1);
  document.getElementById('fieldCapacity').value = upsStatus.capacity;
  document.getElementById('fieldEsr').value = upsStatus.esr;
  document.getElementById('fieldSoc').value = upsStatus.soc;
  document.getElementById('fieldBoardTemperature').value = upsStatus.ucTemperature;
  document.getElementById('checkDevCharging').checked = upsStatus.deviceStatus & 0x01;
  document.getElementById('checkDevDischarging').checked = upsStatus.deviceStatus & 0x02;
  document.getElementById('checkDevPowerPresent').checked = upsStatus.deviceStatus & 0x04;
  document.getElementById('checkDevBatteryPresent').checked = upsStatus.deviceStatus & 0x08;
  document.getElementById('checkDevShutdownSet').checked = upsStatus.deviceStatus & 0x10;
  document.getElementById('checkDevOverCurrent').checked = upsStatus.deviceStatus & 0x20;
  document.getElementById('fieldSeries').value = upsStatus.series;
  document.getElementById('fieldType').value = upsStatus.batteryType;
  document.getElementById('fieldHardware').value = upsStatus.hwRevision;
  document.getElementById('fieldFirmware').value = upsStatus.firmware;
}

/*
 * Start application as soon as DOM is fully loaded
 */
document.addEventListener('DOMContentLoaded', () => {
  serverCommunicationWorker.onmessage = (e) => {
    const msg = e.data;
    switch (msg.cmd) {
      case 'connected':
        document.getElementById('connectionSpinner').classList.add('d-none');
        break;
      case 'disconnected':
        document.getElementById('connectionSpinner').classList.remove('d-none');
        break;
      case 'data':
        UpdateGui(msg.data);
        break;
      default:
        console.error(`Unknown command: ${msg.cmd}`);
    }
  };

  /*
   * Finally, connect to UPS websocket server
   */
  serverCommunicationWorker.postMessage({
    cmd: 'connect',
    data: null
  });
});
