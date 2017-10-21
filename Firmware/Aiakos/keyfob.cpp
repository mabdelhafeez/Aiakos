#ifndef ARDUINO_SAM_DUE

#include "keyfob.h"

static void bleEvent(bleControl::EVENT ev);
static void alertLevelEvent(byte *value, byte& length);
static void rfidEvent(byte *value, byte& length);
extern bool readDataSer(byte* data, byte& length);
extern bool writeDataSer(byte* data, byte length);

namespace {
byte payloadKryptoKnight[]={0xFE, 0xDC, 0xBA, 0x98};
Configuration* cfg;

//Private UUIDs have been generated by: https://www.uuidgenerator.net/version4
btCharacteristic ble_char_rfid("f1a87912-5950-479c-a5e5-b6cc81cd0502",               //private service
                               "855b1938-83e2-4889-80b7-ae58fcd0e6ca",               //private characteristic
                               btCharacteristic::WRITE,Configuration::RFID_KEY_SIZE, //properties+length
                               btCharacteristic::ENCR_W,                             //security
                               rfidEvent);                                           //eventhandler
//https://www.bluetooth.com/specifications/gatt/services
//https://www.bluetooth.com/specifications/gatt/characteristics
btCharacteristic ble_char_ias_alertLevel("1802",                                  //IAS Alert Service
                                         "2A06",                                  //Alert Level characteristic
                                         btCharacteristic::WRITE_WOUT_RESP, 1,    //properties+length
                                         btCharacteristic::NOTHING,               //security
                                         alertLevelEvent);
btCharacteristic* _localCharacteristics[2]={&ble_char_rfid, &ble_char_ias_alertLevel};
KeyFob* thiskeyfob;
bool bConnected;
unsigned long connectionTimeout;
const unsigned long CONNECTION_TIMEOUT=30000;
}

KeyFob::KeyFob(byte ownAddress,
               Configuration* config,
               RH_RF95 *prhLora,
               RH_Serial *prhSerial,
               byte buttonPin,
               byte cableDetectPin,
               bleControl* pble,
               byte tonePin):
    LoRaDevice(ownAddress, prhLora, prhSerial, cableDetectPin),
    buttonPin(buttonPin),
    serProtocol(ECDHCOMM),
    _ble(pble),
    _blePair(writeDataSer, readDataSer, pble, Configuration::RFID_KEY_SIZE),
    tonePin(tonePin)
{
    pushButton = Bounce();
    cfg=config;
    setPeerAddress(1);
    _ble->setEventListener(bleEvent);
    thiskeyfob=this;
}

/* Getting status of some GPIOs as early as possible after startup.
 * This allows to check what is the wakeup source.
 * The BLE connection only takes 280ms, it takes 460ms to get in the "Keyfob::setup()" function, so we would
 * always be too late to detect it.
 * The same holds for the pushbutton.  If the user only gives it a short push, the "Keyfob::setup()" function
 * would be too late to detect it.
 */
void KeyFob::getInitialPinStates()
{
    LoRaDevice::getInitialPinStates();
    loopmode=(!cableDetect.read() ? PAIRING : NORMAL);
    pinMode(buttonPin, INPUT);
    pushButton.attach(buttonPin);
    pushButton.interval(100); // interval in ms
    //Find wakeup source : pushbutton or BLE-module
    if(!pushButton.read())
    {
        wakeupsource=PUSHBUTTON;
        return;
    }
    if(_ble->isConnected())
    {
        wakeupsource=BLE_CONNECTION;
        return;
    }
}


bool KeyFob::setup()
{
    bool rfidKeyOk=false;

    if(!LoRaDevice::setup())    //LoRa device must be setup or we won't be able to put it to sleep.
    {
        return false;
    }
    if(!wokeUpFromStandby())
    {
        sleep();
    }
    switch(loopmode)
    {
    case PAIRING:
        ecdh.reset();
        if((!initBlePeripheral(rfidKeyOk)) ||  (!_ble->startAdvertizement(5000)))
        {
            debug_println("Ble init failed.");
            return false;
        }
        debug_println("Starting BLE pairing...");
        serProtocol=BLE_BOND;
        setPeerAddress(3);
        if(_blePair.startPairing())
        {
            return true;
        }
        debug_println("Starting ECDH pairing...");
        serProtocol=ECDHCOMM;
        setPeerAddress(1);
        k.reset();
        if(!ecdh.startPairing())
        {
            debug_println("ECDH pairing doesn't start");
            return false;
        }
        break;
    case NORMAL:
        switch(wakeupsource)
        {
        case NO_SOURCE:
            debug_println("No valid wakeup source");
            return false;
        case BLE_CONNECTION:
            debug_println("BleConnection waking");
            if(!initBlePeripheral(rfidKeyOk))
            {
                debug_println("Ble init failed.");
                return false;
            }
            if(!rfidKeyOk)
            {
                //Check if we're woken by an alert level
                byte value;
                byte length;
                if(_ble->readLocalCharacteristic(&ble_char_ias_alertLevel, &value, length) && length==1 && value==alert.ALERT_VALUE)
                {
                    debug_println("Start IAS");
                    alert.state=START;
                    return true;
                }
                return false;
            }
            //no break here
        case PUSHBUTTON:
            debug_println("Pushbutton waking");
            debug_println("Initiator starts authentication");
            if(!k.sendMessage(payloadKryptoKnight,sizeof(payloadKryptoKnight), cfg->getDefaultId(), cfg->getIdLength(), cfg->getDefaultKey()))
            {
                debug_println("Sending message failed.");
                return false;
            }
            break;
        }
        break;
    }
    return true;
}

void KeyFob::loop()
{
    if(millis()-connectionTimeout>CONNECTION_TIMEOUT)
    {
        if(bConnected)
        {
            debug_println("Connection timeout");
            _ble->disconnect(3000);
        }
        connectionTimeout=millis();
    }
    switch(loopmode)
    {
    case PAIRING:
        switch(serProtocol)
        {
        case ECDHCOMM:
            switch(ecdh.loop())
            {
            case EcdhComm::AUTHENTICATION_OK:
                debug_println("Securely paired");
                cfg->addKey(ecdh.getRemoteId(), ecdh.getMasterKey());
                break;
            case EcdhComm::NO_AUTHENTICATION:
                sleep();
                break;
            case EcdhComm::AUTHENTICATION_BUSY:
                break;
            case EcdhComm::UNKNOWN_DATA:
                serProtocol=UNKNOWN;
                break;
            }
            break;
        case UNKNOWN:
            //find out the correct protocol
            break;
        case BLE_BOND:
            switch(_blePair.loop())
            {
            case BlePairing::AUTHENTICATION_OK:
                debug_println("Bonded with bike.");
                _ble->disconnect(3000);
                if(!storeBleData())
                {
                    debug_println("rfidkey can't be stored");
                    return;
                }
                break;
            case BlePairing::NO_AUTHENTICATION:
                mgrSer.resetDatagram();
                break;
            case BlePairing::AUTHENTICATION_BUSY:
                break;
            }
            break;
        }
        break;
    case NORMAL:
        switch(k.loop())
        {
        case KryptoKnightComm::AUTHENTICATION_AS_INITIATOR_OK:
            debug_println("Message received by peer and acknowledged");
            break;
        case KryptoKnightComm::NO_AUTHENTICATION:
            if(alert.state!=RUNNING && alert.state!=START)sleep();
            break;
        }
        if(_ble->isConnected())
        {
            _ble->loop();
        }
        byte value=0;
        switch(alert.state)
        {
        case START:
            if(_ble->writeLocalCharacteristic(&ble_char_ias_alertLevel, &value, 1))
            {
                alert.state=RUNNING;
                alert.pulseCounter=0;
            }
            //no break;
        case RUNNING:
            if(millis()>alert.ulTimer+alert.PULSE_PERIOD)
            {
                debug_println("pulse");
                if(alert.pulseCounter++==alert.PULSE_COUNT)
                {
                    alert.state=STOPPED;
                }else
                {
                    alert.ulTimer=millis();
                    tone(tonePin, alert.TONE_FREQUENCY, alert.PULSE_LENGTH);
                }
            }
            break;
        case STOPPED:
            sleep();
            break;
        default:
            break;
        }
        break;
    }
}

void KeyFob::event(bleControl::EVENT ev)
{
    connectionTimeout=millis();
    switch(ev)
    {
    case bleControl::EV_PASSCODE_WANTED:
        _blePair.eventPasscodeInputRequested();
        break;
    case bleControl::EV_CONNECTION_DOWN:
        debug_println("Connection down");
        //After connection goes down, advertizing must be restarted or the module will no longer be connectable.
        //Not doing it here, because we will go to sleep and that routine will take care of it.
        bConnected=false;
        sleep();
        break;
    case bleControl::EV_CONNECTION_UP:
        debug_println("Connection up");
        connectionTimeout=millis();
        bConnected=true;
        break;
    case bleControl::EV_BONDING_BONDED:
        _blePair.eventBondingBonded();
        break;
    default:
        debug_print("Unknown event: ");
        debug_println(ev, DEC);
        break;
    }

}

void bleEvent(bleControl::EVENT ev)
{
    thiskeyfob->event(ev);
}

bool KeyFob::initBlePeripheral(bool& rfidKeyVerified)
{
    const char BT_NAME_KEYFOB[]="AiakosKeyFob";
    rfidKeyVerified=false;
    debug_println("Starting BLE init");
    unsigned long ulStart=millis();

    if(!_ble->init(9600))
    {
        debug_println("RN4020 not set up");
        return false;
    }

    ble_char_rfid.setHandle(cfg->getRfidHandle());
    ble_char_ias_alertLevel.setHandle(cfg->getIasHandle());

    byte value[Configuration::RFID_KEY_SIZE];
    byte length;
    if(!_ble->readLocalCharacteristic(&ble_char_rfid, value, length))
    {
        //Module not yet correctly configured
        if(!_ble->programPeripheral()
                || !_ble->addLocalCharacteristics(_localCharacteristics, 2)
                || !_ble->setBluetoothDeviceName(BT_NAME_KEYFOB)
                || !_ble->reboot())
        {
            return false;
        }
        word handle=_ble->getLocalHandle(&ble_char_rfid);
        cfg->setRfidHandle(handle);
        handle=_ble->getLocalHandle(&ble_char_ias_alertLevel);
        cfg->setIasHandle(handle);
    }
    else
    {
        rfidKeyVerified=verifyRfidKey(value);
    }

    if(!_ble->beginPeripheral(_localCharacteristics, 2))
    {
        return false;
    }

    debug_print("BLE setup took [ms]: ");debug_println(millis()-ulStart, DEC);
    return true;
}

void KeyFob::alertEvent(byte* value, byte &length)
{
    debug_print("Characteristic changed to: ");
    debug_printArray(value, length);
    if(length==1)
    {
        if(*value==0)alert.state=STOPPED;
        if(*value==alert.ALERT_VALUE)alert.state=START;
    }
}

void KeyFob::rfidEvent(byte* value, byte &length)
{
    if(!_ble->isBonded() || !_ble->isSecured())
    {
        debug_println("Illegal write of event");
        return;
    }
    verifyRfidKey(value);
}

bool KeyFob::storeBleData()
{
    byte rfidkey[Configuration::RFID_KEY_SIZE];
    if(!_blePair.getRfidKey(rfidkey))
    {
        return false;
    }
    if(!_ble->beginPeripheral(_localCharacteristics, 2))
    {
        return false;
    }
    return cfg->setRfidKey(rfidkey);
}

void KeyFob::sleep()
{
    debug_println("Zzz...zzz...");
    if(_ble->isInitialized())
    {
        if(_ble->isConnected())
        {
            _ble->disconnect(3000);
        }
        _ble->startAdvertizement(5000);
        _ble->sleep();
    }
    rhLoRa->sleep();
#ifdef DEBUG
    delay(500);
#endif
    sleepAndWakeUp(STANDBY);
}

bool KeyFob::verifyRfidKey(byte* value)
{
    debug_printArray(value, Configuration::RFID_KEY_SIZE);
    if(!cfg->equalsRfidKey(value))
    {
        return false;
    }
    memset(value, 0, Configuration::RFID_KEY_SIZE);
    if(!_ble->writeLocalCharacteristic(&ble_char_rfid, value, Configuration::RFID_KEY_SIZE))
    {
        return false;
    }
    debug_println("Correct key received.");
    return true;
}

void alertLevelEvent(byte* value, byte &length)
{
    thiskeyfob->alertEvent(value, length);
}

void rfidEvent(byte* value, byte &length)
{
    thiskeyfob->rfidEvent(value, length);
}
#endif
