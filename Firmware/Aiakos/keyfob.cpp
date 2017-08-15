#include "keyfob.h"
#define DEBUG

void bleEvent(bleControl::EVENT ev);
void alertLevelEvent(byte *value, byte& length);
extern bool readDataSer(byte** data, byte& length);
extern bool writeDataSer(byte* data, byte length);

namespace {
byte payload[4]={0xFE, 0xDC, 0xBA, 0x98};
Configuration* cfg;

//Private UUIDs have been generated by: https://www.uuidgenerator.net/version4
btCharacteristic rfid_key("f1a87912-5950-479c-a5e5-b6cc81cd0502",        //private service
                          "855b1938-83e2-4889-80b7-ae58fcd0e6ca",        //private characteristic
                          btCharacteristic::WRITE_WOUT_RESP,5,           //properties+length
                          btCharacteristic::ENCR_W);                     //security
//https://www.bluetooth.com/specifications/gatt/services
//https://www.bluetooth.com/specifications/gatt/characteristics
btCharacteristic ias_alertLevel("1802",                                  //IAS Alert Service
                                "2A06",                                  //Alert Level characteristic
                                btCharacteristic::WRITE_WOUT_RESP, 1,    //properties+length
                                btCharacteristic::NOTHING,                //security
                                alertLevelEvent);
btCharacteristic* _localCharacteristics[2]={&rfid_key, &ias_alertLevel};
KeyFob* thiskeyfob;
bool bConnected;
}

KeyFob::KeyFob(byte ownAddress,
               Configuration* config,
               RH_RF95 *prhLora,
               RH_Serial *prhSerial,
               byte buttonPin,
               byte cableDetectPin,
               bleControl* pble):
    LoRaDevice(ownAddress, prhLora, prhSerial, cableDetectPin),
    BUTTON_PIN(buttonPin),
    serProtocol(ECDHCOMM),
    _ble(pble),
    _blePair(writeDataSer, readDataSer, pble)
{
    pushButton = Bounce();
    cfg=config;
    setPeerAddress(1);
    _ble->setEventListener(bleEvent);
    thiskeyfob=this;
}

bool KeyFob::setup()
{
    if(!LoRaDevice::setup())
    {
        return false;
    }
    if(!initBlePeripheral())
    {
        debug_println("Ble init failed.");
        return false;
    }
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pushButton.attach(BUTTON_PIN);
    pushButton.interval(100); // interval in ms
    return true;
}


void KeyFob::loop()
{
    cableDetect.update();
    pushButton.update();
    if(!cableDetect.read())
    {
        //Secure pairing mode
        if(pushButton.fell())
        {
            debug_println("Starting BLE pairing...");
            serProtocol=BLE_BOND;
            setPeerAddress(3);
            if(!_blePair.startPairing())
            {
                debug_println("Starting ECDH pairing...");
                serProtocol=ECDHCOMM;
                setPeerAddress(1);
                k.reset();
                if(!ecdh.startPairing())
                {
                    debug_println("ECDH pairing doesn't start");
                }
                return;
            }
        }
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
            _blePair.loop();
            break;
        }
    }else
    {
        //Authenticating remote peer mode
        if(pushButton.fell())
        {
            ecdh.reset();
            debug_println("Initiator starts authentication");
            if(!k.sendMessage(payload,sizeof(payload), cfg->getDefaultId(), cfg->getIdLength(), cfg->getDefaultKey()))
            {
                Serial.println("Sending message failed.");
                return;
            }
        }
        if(k.loop()==KryptoKnightComm::AUTHENTICATION_AS_INITIATOR_OK)
        {
            Serial.println("Message received by peer and acknowledged");
        }
    }
}

void  KeyFob::eventPasscodeInputRequested()
{
    _blePair.eventPasscodeInputRequested();
}


void bleEvent(bleControl::EVENT ev)
{
    switch(ev)
    {
    case bleControl::EV_PASSCODE_WANTED:
        thiskeyfob->eventPasscodeInputRequested();
        break;
    case bleControl::EV_CONNECTION_DOWN:
        debug_println("Connection down");
        bConnected=false;
        break;
    case bleControl::EV_CONNECTION_UP:
        debug_println("Connection up");
        bConnected=true;
        break;
    default:
        debug_print("Unknown event: ");
        debug_println(ev, DEC);
        break;
    }
}

bool KeyFob::initBlePeripheral()
{
    char dataname[20];
    const char BT_NAME_KEYFOB[]="AiakosKeyFob";

    if(!_ble->init())
    {
        debug_println("RN4020 not set up");
        return false;
    }
    if(!_ble->getBluetoothDeviceName(dataname))
    {
        return false;
    }
    //Check if programming the settings has already been done.  If yes, we don't have to set them again.
    //This is check is performed by verifying if the last setting command has finished successfully:
    if(strncmp(dataname,BT_NAME_KEYFOB, strlen(BT_NAME_KEYFOB)))
    {
        //Module not yet correctly configured
        if(!_ble->programPeripheral())
        {
            return false;
        }
        if(!_ble->addLocalCharacteristics(_localCharacteristics,2))
        {
            return false;
        }
        if(!_ble->setBluetoothDeviceName(BT_NAME_KEYFOB))
        {
            return false;
        }
    }
    return _ble->beginPeripheral(_localCharacteristics,2);
}

void alertLevelEvent(byte* value, byte &length)
{
    debug_print("Characteristic changed to: ");
    debug_printArray(value, length);
}


