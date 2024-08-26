#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <utils/optionlist.h>
#include <atomic>
#include <kcsdr.h>

SDRPP_MOD_INFO{
    /* Name:            */ "kcsdr_source",
    /* Description:     */ "kcsdr Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 0, 0,
    /* Max instances    */ -1
};

sdr_obj* sdr;
sdr_api* sdr_handler;
int16_t* d_buf = new int16_t[2 * 409600];

#define CONCAT(a, b) ((std::string(a) + b).c_str())

class KCSDRSourceModule : public ModuleManager::Instance {
public:
    KCSDRSourceModule(std::string name) {
        this->name = name;

        sampleRate = 40000000.0;
        this->bufferSize = sampleRate / 200;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        // Refresh devices
        refresh();

        // Select first (TODO: Select from config)
        select("");

        sigpath::sourceManager.registerSource("KCSDR", &handler);
    }

    ~KCSDRSourceModule() {
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    void refresh() {
        int i = 0;

        sdr_handler = kcsdr_init();

        devices.clear();

        if(sdr = sdr_handler->find(KC_908_1)) {
            char serial[64];
            sprintf(serial, "%s [%s]", sdr->name, sdr->serial_num);

            devices.define(serial, serial, i++);
        }
    }

    void select(const std::string& serial) {
        // If there are no devices, give up
        if (devices.empty()) {
            selectedSerial.clear();
            return;
        }

        // If the serial was not found, select the first available serial
        if (!devices.keyExists(serial)) {
            select(devices.key(0));
            return;
        }

        // Get the menu ID
        devId = devices.keyId(serial);
        selectedDevIndex = devices.value(devId);

        // Update the samplerate
        core::setInputSampleRate(sampleRate);

        // Save serial number
        selectedSerial = serial;

    }

    static void menuSelected(void* ctx) {
        KCSDRSourceModule* _this = (KCSDRSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("KCSDRSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        KCSDRSourceModule* _this = (KCSDRSourceModule*)ctx;
        flog::info("KCSDRSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        KCSDRSourceModule* _this = (KCSDRSourceModule*)ctx;
        if (_this->running) { return; }

        sdr_handler->rx_amp(sdr, 0);
        sdr_handler->rx_ext_amp(sdr, 0);
        sdr_handler->rx_bw(sdr, 40000000);
        sdr_handler->rx_start(sdr);

        // Start worker
        _this->run = true;
        _this->workerThread = std::thread(&KCSDRSourceModule::worker, _this);

        _this->running = true;
        flog::info("KCSDRSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
    }

    static void tune(double freq, void* ctx) {
        sdr_handler->rx_freq(sdr, (int)freq);
    }

    static void menuHandler(void* ctx) {
        KCSDRSourceModule* _this = (KCSDRSourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_kcsdr_dev_sel_", _this->name), &_this->devId, _this->devices.txt)) {
            _this->select(_this->devices.key(_this->devId));
            core::setInputSampleRate(_this->sampleRate);
            // TODO: Save
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_kcsdr_refr_", _this->name))) {
            _this->refresh();
            _this->select(_this->selectedSerial);
            core::setInputSampleRate(_this->sampleRate);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Amp");
        SmGui::FillWidth();

        int minimum = sdr->port[1].att.minimum;
        int maximum = sdr->port[1].att.maximum * 1;

        if (SmGui::SliderInt(CONCAT("##_kcsdr_amp_", _this->name), &_this->ampLvl, minimum, maximum)) {
            if (_this->running) {
                sdr_handler->rx_ext_amp(sdr, _this->ampLvl);
            }
        }

        SmGui::LeftLabel("Att");
        SmGui::FillWidth();

        int att_minimum = sdr->port[1].att.minimum;
        int att_maximum = sdr->port[1].att.maximum * 1;

        if (SmGui::SliderInt(CONCAT("##_kcsdr_att_", _this->name), &_this->attLvl, att_minimum, att_maximum)) {
            if (_this->running) {
                sdr_handler->rx_att(sdr, _this->attLvl);
            }
        }

        SmGui::LeftLabel("IF Gain");
        SmGui::FillWidth();

        int ifgain_minimum = sdr->port[1].ifgain.minimum;
        int ifgain_maximum = sdr->port[1].ifgain.maximum;

        if (SmGui::SliderInt(CONCAT("##_kcsdr_ifgain_", _this->name), &_this->ifGainLvl, ifgain_minimum, ifgain_maximum)) {
            if (_this->running) {
                sdr_handler->rx_amp(sdr, _this->ifGainLvl);
            }
        }

    }

    void applyProfile() {
    }

    void worker() {
        // Allocate sample buffer
        int realSamps = bufferSize*2;

        // Define number of buffers per swap to maintain 200 fps
        int maxBufCount = STREAM_BUFFER_SIZE / bufferSize;
        int bufCount = (sampleRate / bufferSize) / 200;
        if (bufCount <= 0) { bufCount = 1; }
        if (bufCount > maxBufCount) { bufCount = maxBufCount; }
        int count = 0;

        flog::debug("Swapping will be done {} buffers at a time", bufCount);

        // Worker loop
        while (run) {
            // Read samples
            devMtx.lock();

            bool ret = false;
            do
            {
                ret = sdr_handler->read(sdr, (uint8_t *)d_buf, realSamps * sizeof(uint16_t));
            }while(ret == false);

            // Read data
            devMtx.unlock();

            volk_16i_s32f_convert_32f((float*)&stream.writeBuf[(count++)*bufferSize], (int16_t*)d_buf, 8192.0f, realSamps);

            // Send them off if we have enough
            if (count >= bufCount) {
                count = 0;
                if (!stream.swap(bufferSize*bufCount)) { break; }
            }
        }
    }

    OptionList<std::string, int> devices;

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;

    int devId = 0;
    int srId = 0;
    int minRef = -100;
    int maxRef = 7;
    int portId = 0;
    int gainStratId = 0;
    int preampModeId = 0;
    int loModeId = 0;
    int ifGainLvl = 0;
    int ampLvl = 0;
    int attLvl = 0;
    bool ifAgc = false;
    std::string selectedSerial;
    int selectedDevIndex;

    void* openDev;

    int bufferSize;
    std::thread workerThread;
    std::atomic<bool> run = false;
    std::mutex devMtx;
    bool sampsInt8;
};

MOD_EXPORT void _INIT_() {
    // Nothing here
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new KCSDRSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (KCSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    // Nothing here
}
