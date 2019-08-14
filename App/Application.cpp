//
//    Application.cpp: SigDigger main class
//    Copyright (C) 2019 Gonzalo José Carracedo Carballal
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Lesser General Public License as
//    published by the Free Software Foundation, either version 3 of the
//    License, or (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful, but
//    WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public
//    License along with this program.  If not, see
//    <http://www.gnu.org/licenses/>
//

#include <Suscan/Library.h>
#include <fcntl.h>

#include "Application.h"

#include <QMessageBox>

using namespace SigDigger;

Application::Application(QWidget *parent) : QMainWindow(parent), ui(this)
{
  this->mediator = new UIMediator(this, &this->ui);
}

Suscan::Object &&
Application::getConfig(void)
{
  return this->mediator->getConfig()->serialize();
}

void
Application::refreshConfig(void)
{
  this->mediator->saveGeometry();
}

void
Application::run(Suscan::Object const &config)
{
  this->ui.postLoadInit(this);

  this->mediator->loadSerializedConfig(config);

  this->mediator->setState(UIMediator::HALTED);

  this->connectUI();

  this->show();
}

AsyncDataSaver *
Application::getSaver(void) const
{
  return this->dataSaver.get();
}

SUPRIVATE SUBOOL
onBaseBandData(
    void *privdata,
    suscan_analyzer_t *,
    const SUCOMPLEX *samples,
    SUSCOUNT length)
{
  Application *app = static_cast<Application *>(privdata);
  AsyncDataSaver *saver;

  if ((saver = app->getSaver()) != nullptr)
    saver->write(samples, length);

  return SU_TRUE;
}

void
Application::uninstallDataSaver()
{
  this->dataSaver = nullptr;
}

void
Application::connectDataSaver()
{
  this->connect(
        this->dataSaver.get(),
        SIGNAL(stopped()),
        this,
        SLOT(onSaveError()));

  this->connect(
        this->dataSaver.get(),
        SIGNAL(swamped()),
        this,
        SLOT(onSaveSwamped()));

  this->connect(
        this->dataSaver.get(),
        SIGNAL(dataRate(qreal)),
        this,
        SLOT(onSaveRate(qreal)));

  this->connect(
        this->dataSaver.get(),
        SIGNAL(commit()),
        this,
        SLOT(onCommit()));
}

void
Application::installDataSaver(int fd)
{
  if (this->dataSaver.get() == nullptr && this->analyzer.get() != nullptr) {
    this->dataSaver = std::make_unique<AsyncDataSaver>(fd, this);
    this->dataSaver->setSampleRate(this->mediator->getProfile()->getSampleRate());
    if (!this->filterInstalled) {
      this->analyzer->registerBaseBandFilter(onBaseBandData, this);
      this->filterInstalled = true;
    }
    this->connectDataSaver();
  }
}

void
Application::setAudioInspectorParams(
    unsigned int rate,
    SUFLOAT cutOff,
    SUFLOAT volume,
    unsigned int demod)
{
  if (this->audioConfigured) {
    Suscan::Config cfg(this->audioCfgTemplate);
    cfg.set("audio.cutoff", cutOff);
    cfg.set("audio.volume", volume / 20);
    cfg.set("audio.sample-rate", static_cast<uint64_t>(rate));
    cfg.set("audio.demodulator", static_cast<uint64_t>(demod));
    this->analyzer->setInspectorConfig(this->audioInspHandle, cfg, 0);
    this->assertAudioInspectorLo();
  } else {
    this->delayedRate    = rate;
    this->delayedCutOff  = cutOff;
    this->delayedVolume  = volume;
    this->delayedDemod   = demod;
  }
}

bool
Application::openAudio(unsigned int rate)
{
  bool opened = false;

  if (this->mediator->getState() == UIMediator::RUNNING) {
    if (this->playBack == nullptr) {
      try {
        Suscan::Channel ch;
        SUFREQ bw = SIGDIGGER_AUDIO_INSPECTOR_BANDWIDTH;

        if (rate > bw)
          rate = static_cast<unsigned int>(floor(bw));

        this->playBack = std::make_unique<AudioPlayback>("default", rate);
        this->audioSampleRate = this->playBack->getSampleRate();
        this->lastAudioLo = this->getAudioInspectorLo();

        if (bw > this->analyzer->getSampleRate() / 2)
          bw = this->analyzer->getSampleRate() / 2;

        ch.bw    = bw;
        ch.ft    = 0;
        ch.fc    = this->getAudioInspectorLo();
        ch.fLow  = -.5 * bw;
        ch.fHigh = .5 * bw;

        this->maxAudioBw = bw;

        this->analyzer->openPrecise(
              "audio",
              ch,
              SIGDIGGER_AUDIO_INSPECTOR_REQID);

        this->setAudioInspectorParams(
              this->audioSampleRate,
              this->ui.audioPanel->getCutOff(),
              this->ui.audioPanel->getVolume(),
              this->ui.audioPanel->getDemod() + 1);
        opened = true;
      } catch (Suscan::Exception const &e) {
        QMessageBox::critical(
                  this,
                  "Internal Suscan exception",
                  "Failed to open inspector. Error was:\n\n<pre>"
                  + QString(e.what()) + "</pre>",
                  QMessageBox::Ok);
        this->playBack = nullptr;
      } catch (std::runtime_error const &e) {
        QMessageBox::warning(
                  this,
                  "Failed to open soundcard device",
                  "Cannot open audio device. Error was:\n\n<pre>"
                  + QString(e.what()) + "</pre>",
                  QMessageBox::Ok);
      }
    }
  }

  return opened;
}

void
Application::closeAudio(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING
      && this->audioInspectorOpened)
    this->analyzer->closeInspector(this->audioInspHandle, 0);
  this->audioInspectorOpened = false;
  this->audioSampleRate = 0;
  this->audioInspHandle = 0;
  this->playBack = nullptr;
  this->audioConfigured = false;
}

SUFREQ
Application::getAudioInspectorBandwidth(void) const
{
  SUFREQ bw = this->ui.spectrum->getBandwidth();

  if (bw > this->maxAudioBw)
    bw = this->maxAudioBw;
  else if (bw < 1)
    bw = 1;

  return bw;
}

SUFREQ
Application::getAudioInspectorLo(void) const
{
  SUFREQ lo = this->ui.spectrum->getLoFreq();
  SUFREQ bw = this->getAudioInspectorBandwidth();

  if (this->ui.audioPanel->getDemod() == AudioDemod::USB)
    lo += .5 * bw;
  else if (this->ui.audioPanel->getDemod() == AudioDemod::LSB)
    lo -= .5 * bw;

  return lo;
}


void
Application::connectUI(void)
{
  connect(
        this->mediator,
        SIGNAL(captureStart()),
        this,
        SLOT(onCaptureStart()));

  connect(
        this->mediator,
        SIGNAL(captureEnd()),
        this,
        SLOT(onCaptureStop()));

  connect(
        this->mediator,
        SIGNAL(profileChanged()),
        this,
        SLOT(onProfileChanged()));

  connect(
        this->mediator,
        SIGNAL(frequencyChanged(qint64)),
        this,
        SLOT(onFrequencyChanged(qint64)));

  connect(
        this->mediator,
        SIGNAL(toggleRecord(void)),
        this,
        SLOT(onToggleRecord(void)));

  connect(
        this->mediator,
        SIGNAL(throttleConfigChanged(void)),
        this,
        SLOT(onThrottleConfigChanged(void)));

  connect(
        this->mediator,
        SIGNAL(gainChanged(QString, float)),
        this,
        SLOT(onGainChanged(QString, float)));

  connect(
        this->mediator,
        SIGNAL(requestOpenInspector(void)),
        this,
        SLOT(onOpenInspector(void)));

  connect(
        this->mediator,
        SIGNAL(toggleDCRemove(void)),
        this,
        SLOT(onToggleDCRemove(void)));

  connect(
        this->mediator,
        SIGNAL(toggleIQReverse(void)),
        this,
        SLOT(onToggleIQReverse(void)));

  connect(
        this->mediator,
        SIGNAL(toggleAGCEnabled(void)),
        this,
        SLOT(onToggleAGCEnabled(void)));

  connect(
      this->mediator,
        SIGNAL(analyzerParamsChanged(void)),
        this,
        SLOT(onParamsChanged(void)));

  connect(
        this->mediator,
        SIGNAL(loChanged(qint64)),
        this,
        SLOT(onLoChanged(qint64)));

  connect(
        this->mediator,
        SIGNAL(bandwidthChanged(qreal)),
        this,
        SLOT(onBandwidthChanged(qreal)));

  connect(
        this->mediator,
        SIGNAL(audioChanged(void)),
        this,
        SLOT(onAudioChanged(void)));
}

void
Application::connectAnalyzer(void)
{
  connect(
        this->analyzer.get(),
        SIGNAL(halted(void)),
        this,
        SLOT(onAnalyzerHalted(void)));

  connect(
        this->analyzer.get(),
        SIGNAL(eos(void)),
        this,
        SLOT(onAnalyzerEos(void)));

  connect(
        this->analyzer.get(),
        SIGNAL(read_error(void)),
        this,
        SLOT(onAnalyzerReadError(void)));

  connect(
        this->analyzer.get(),
        SIGNAL(psd_message(const Suscan::PSDMessage &)),
        this,
        SLOT(onPSDMessage(const Suscan::PSDMessage &)));

  connect(
        this->analyzer.get(),
        SIGNAL(inspector_message(const Suscan::InspectorMessage &)),
        this,
        SLOT(onInspectorMessage(const Suscan::InspectorMessage &)));

  connect(
        this->analyzer.get(),
        SIGNAL(samples_message(const Suscan::SamplesMessage &)),
        this,
        SLOT(onInspectorSamples(const Suscan::SamplesMessage &)));
}

QString
Application::getLogText(void)
{
  QString text = "";
  std::lock_guard<Suscan::Logger> guard(*Suscan::Logger::getInstance());

  for (const auto &p : *Suscan::Logger::getInstance()) {
    switch (p.severity) {
      case SU_LOG_SEVERITY_CRITICAL:
        text += "critical: ";
        break;

      case SU_LOG_SEVERITY_DEBUG:
        text += "debug: ";
        break;

      case SU_LOG_SEVERITY_ERROR:
        text += "error: ";
        break;

      case SU_LOG_SEVERITY_INFO:
        text += "info: ";
        break;

      case SU_LOG_SEVERITY_WARNING:
        text += "warning: ";
        break;
    }

    text += p.message.c_str();
  }

  return text;
}

void
Application::startCapture(void)
{
  try {
    this->filterInstalled = false;

    if (this->mediator->getState() == UIMediator::HALTED) {
      std::unique_ptr<Suscan::Analyzer> analyzer;
      //int maxIfFreq;

      if (this->mediator->getProfile()->getType() == SUSCAN_SOURCE_TYPE_SDR) {
        if (this->mediator->getProfile()->getSampleRate() > SIGDIGGER_MAX_SAMPLE_RATE) {
          QMessageBox::StandardButton reply;
          reply = QMessageBox::question(
                this,
                "Sample rate too high",
                "The sample rate of profile \""
                + QString::fromStdString(this->mediator->getProfile()->label())
                + "\" is unusually big ("
                + QString::number(this->mediator->getProfile()->getSampleRate())
                + "). Temporarily reduce it to "
                + QString::number(SIGDIGGER_MAX_SAMPLE_RATE)
                + "?",
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

          if (reply == QMessageBox::Yes)
            this->mediator->getProfile()->setSampleRate(SIGDIGGER_MAX_SAMPLE_RATE);
          else if (reply == QMessageBox::Cancel)
            return;
        }
      }

      // Flush log messages from here
      Suscan::Logger::getInstance()->flush();

      // Allocate objects
      if (this->mediator->getProfile()->instance == nullptr) {
        QMessageBox::warning(
                  this,
                  "SigDigger error",
                  "No source defined yet. Please define a source in the settings window.",
                  QMessageBox::Ok);
        return;
      }

      analyzer = std::make_unique<Suscan::Analyzer>(
            *this->mediator->getAnalyzerParams(),
            *this->mediator->getProfile());

      // Enable throttling, if requested
      if (this->ui.sourcePanel->isThrottleEnabled())
        analyzer->setThrottle(this->ui.sourcePanel->getThrottleRate());

      analyzer->setDCRemove(this->ui.sourcePanel->getDCremove());
      analyzer->setIQReverse(this->ui.sourcePanel->getIQReverse());

      if (this->ui.sourcePanel->getAGCEnabled())
        analyzer->setAGC(true);

      // All set, move to application
      this->analyzer = std::move(analyzer);

      // If there is a capture file configured, install data saver
      if (this->ui.sourcePanel->getRecordState()) {
        int fd = this->openCaptureFile();
        if (fd != -1)
          this->installDataSaver(fd);
      }

      this->connectAnalyzer();

      this->mediator->setState(UIMediator::RUNNING);

      if (this->ui.audioPanel->getEnabled())
        this->openAudio(this->ui.audioPanel->getSampleRate());
    }
  } catch (Suscan::Exception &) {
    (void)  QMessageBox::critical(
          this,
          "SigDigger error",
          "Failed to start capture due to errors:<p /><pre>"
          + getLogText()
          + "</pre>",
          QMessageBox::Ok);
    this->mediator->setState(UIMediator::HALTED);
  }
}

void
Application::stopCapture(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING) {
    this->mediator->setState(UIMediator::HALTING);
    this->analyzer.get()->halt();
  }
}

void
Application::restartCapture(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING) {
    this->mediator->setState(UIMediator::RESTARTING);
    this->analyzer.get()->halt();
  }
}

void
Application::onAnalyzerHalted(void)
{
  bool restart = this->mediator->getState() == UIMediator::RESTARTING;

  this->analyzer = nullptr;
  this->uninstallDataSaver();
  this->mediator->setState(UIMediator::HALTED);
  this->mediator->detachAllInspectors();
  this->closeAudio();

  if (restart)
    this->startCapture();
}

void
Application::onAnalyzerEos(void)
{
  (void)  QMessageBox::information(
        this,
        "End of stream",
        "Capture interrupted due to stream end:<p /><pre>"
        + getLogText()
        + "</pre>",
        QMessageBox::Ok);

  this->mediator->setState(UIMediator::HALTED);
  this->mediator->detachAllInspectors();
  this->analyzer = nullptr;
  this->closeAudio();
  this->uninstallDataSaver();
}

void
Application::onPSDMessage(const Suscan::PSDMessage &msg)
{
  this->mediator->feedPSD(msg);
}

void
Application::onInspectorSamples(const Suscan::SamplesMessage &msg)
{
  Inspector *insp;

  if (this->playBack != nullptr
      && msg.getInspectorId() == SIGDIGGER_AUDIO_INSPECTOR_MAGIC_ID) {
    this->playBack->write(msg.getSamples(), msg.getCount());
  } else if ((insp = this->mediator->lookupInspector(msg.getInspectorId())) != nullptr)
    insp->feed(msg.getSamples(), msg.getCount());
}


void
Application::onInspectorMessage(const Suscan::InspectorMessage &msg)
{
  Inspector *insp = nullptr;
  SUFLOAT *data;
  SUSCOUNT len, p;
  Suscan::InspectorId oId;
  float x, max = 0;

  switch (msg.getKind()) {
    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_OPEN:
      // Audio path: set inspector Id
      if (msg.getRequestId() == SIGDIGGER_AUDIO_INSPECTOR_REQID) {
        this->audioInspHandle = msg.getHandle();
        this->audioInspectorOpened = true;
        this->analyzer->setInspectorId(
              msg.getHandle(),
              SIGDIGGER_AUDIO_INSPECTOR_MAGIC_ID,
              0);
        this->analyzer->setInspectorWatermark(
              msg.getHandle(),
              SIGDIGGER_AUDIO_BUFFER_SIZE / 2,
              0);
        this->analyzer->setInspectorBandwidth(
              msg.getHandle(),
              this->getAudioInspectorBandwidth(),
              0);
        if (this->audioCfgTemplate == nullptr)
          SU_ATTEMPT(this->audioCfgTemplate = suscan_config_dup(msg.getCConfig()));

        this->audioConfigured = true;

        /* Set params for good */
        this->setAudioInspectorParams(
              this->audioSampleRate,
              this->delayedCutOff,
              this->delayedVolume,
              this->delayedDemod);
      } else {
        insp = this->mediator->addInspectorTab(msg, oId);
        insp->setAnalyzer(this->analyzer.get());
        this->analyzer->setInspectorId(msg.getHandle(), oId, 0);
      }
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_SPECTRUM:
       if ((insp = this->mediator->lookupInspector(msg.getInspectorId())) != nullptr) {
         data = msg.getSpectrumData();
         len = msg.getSpectrumLength();
         p = len / 2;
         max = 0;

         for (auto i = 0u; i < len; ++i) {
           data[i] = log10f(data[i]);
           if (max < data[i])
             max = data[i];
         }

         max += 5;

         for (auto i = 0u; i < len / 2; ++i) {
           x = data[i];
           data[i] = data[p] - max;
           data[p] = x - max;

           if (++p == len)
             p = 0;
         }

         insp->feedSpectrum(data, len, msg.getSpectrumRate());
       }
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_CLOSE:
      if (this->audioConfigured && this->audioInspHandle == msg.getHandle()) {
        // Do nothing
      } else if ((insp = this->mediator->lookupInspector(msg.getInspectorId())) != nullptr) {
        insp->setAnalyzer(nullptr);
        this->mediator->closeInspectorTab(insp);
      }

      break;

    default:
      // printf("Ignored inspector message of type %d\n", msg.getKind());
      break;
  }
}

void
Application::onAnalyzerReadError(void)
{
  (void)  QMessageBox::critical(
        this,
        "Source error",
        "Capture stopped due to source read error. Last errors were:<p /><pre>"
        + getLogText()
        + "</pre>",
        QMessageBox::Ok);
  this->mediator->setState(UIMediator::HALTED);
  this->analyzer = nullptr;
  this->uninstallDataSaver();
}

Application::~Application()
{
  if (this->audioCfgTemplate != nullptr)
    suscan_config_destroy(this->audioCfgTemplate);

  this->playBack = nullptr;
  this->analyzer = nullptr;
  this->uninstallDataSaver();
}

//////////////////////////////// Slots /////////////////////////////////////////
void
Application::onCaptureStart(void)
{
  this->startCapture();
}

void
Application::onCaptureStop(void)
{
  this->stopCapture();
}

void
Application::onProfileChanged(void)
{
  this->restartCapture();
}

void
Application::onGainChanged(QString name, float val)
{
  if (this->mediator->getState() == UIMediator::RUNNING) {
    this->mediator->getProfile()->setGain(name.toStdString(), val);
    this->analyzer->setGain(name.toStdString(), val);
  }
}

void
Application::onFrequencyChanged(qint64 freq)
{
  this->mediator->getProfile()->setFreq(freq);
  if (this->mediator->getState() == UIMediator::RUNNING)
    this->analyzer->setFrequency(freq);
}

void
Application::onToggleIQReverse(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING)
    this->analyzer->setIQReverse(this->ui.sourcePanel->getIQReverse());
}

void
Application::onToggleDCRemove(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING)
    this->analyzer->setDCRemove(this->ui.sourcePanel->getDCremove());
}

void
Application::onToggleAGCEnabled(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING)
    this->analyzer->setAGC(this->ui.sourcePanel->getAGCEnabled());
}

void
Application::onParamsChanged(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING)
    this->analyzer->setParams(*this->mediator->getAnalyzerParams());
}

void
Application::onOpenInspector(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING) {
    Suscan::Channel ch;

    ch.bw    = this->ui.inspectorPanel->getBandwidth();
    ch.ft    = 0;
    ch.fc    = this->ui.spectrum->getLoFreq();
    ch.fLow  = - .5 * ch.bw;
    ch.fHigh = + .5 * ch.bw;

    this->analyzer->open(this->ui.inspectorPanel->getInspectorClass(), ch, 0);
  }
}

void
Application::onThrottleConfigChanged(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING) {
    if (this->ui.sourcePanel->isThrottleEnabled()) {
      // TODO: Modify dataSaver
      this->analyzer->setThrottle(this->ui.sourcePanel->getThrottleRate());
    } else {
      this->analyzer->setThrottle(0);
    }
  }
}

int
Application::openCaptureFile(void)
{
  int fd = -1;
  QString baseName =
      "sigdigger_"
      + QString::number(this->mediator->getProfile()->getSampleRate())
      + "_"
      + QString::number(this->mediator->getProfile()->getFreq())
      + "_float32_iq.raw";
  std::string fullPath =
      this->ui.sourcePanel->getRecordSavePath() + "/" + baseName.toStdString();

  if ((fd = creat(fullPath.c_str(), 0600)) == -1) {
    QMessageBox::warning(
              this,
              "SigDigger error",
              "Failed to open capture file for writing: " +
              QString(strerror(errno)),
              QMessageBox::Ok);
  }

  return fd;
}

void
Application::onToggleRecord(void)
{
  if (this->ui.sourcePanel->getRecordState()) {
    if (this->mediator->getState() == UIMediator::RUNNING) {
      int fd = this->openCaptureFile();
      if (fd != -1)
        this->installDataSaver(fd);
    }
  } else {
    this->uninstallDataSaver();
    this->mediator->setCaptureSize(0);
  }
}

void
Application::onSaveError(void)
{
  if (this->dataSaver.get() != nullptr) {
    this->uninstallDataSaver();

    QMessageBox::warning(
              this,
              "SigDigger error",
              "Capture file write error. Disk full?",
              QMessageBox::Ok);

    this->mediator->setRecordState(false);
  }
}

void
Application::onSaveSwamped(void)
{
  if (this->dataSaver.get() != nullptr) {
    this->uninstallDataSaver();

    QMessageBox::warning(
          this,
          "SigDigger error",
          "Capture thread swamped. Maybe your storage device is too slow",
          QMessageBox::Ok);

    this->mediator->setRecordState(false);
  }
}

void
Application::onSaveRate(qreal rate)
{
  this->mediator->setIORate(rate);
}

void
Application::onCommit(void)
{
  this->mediator->setCaptureSize(this->dataSaver->getSize());
}

void
Application::onLoChanged(qint64)
{
  if (this->audioConfigured)
    this->assertAudioInspectorLo();
}

void
Application::assertAudioInspectorLo(void)
{
  SUFREQ lo = this->getAudioInspectorLo();

  if (fabs(lo - this->lastAudioLo) > 1e-8) {
    this->analyzer->setInspectorFreq(this->audioInspHandle, lo, 0);
    this->lastAudioLo = lo;
  }
}

void
Application::onBandwidthChanged(qreal)
{
  if (this->audioConfigured) {
    SUFREQ bw;
    bw = this->getAudioInspectorBandwidth();

    this->analyzer->setInspectorBandwidth(this->audioInspHandle, bw, 0);
    this->assertAudioInspectorLo();
  }
}

void
Application::onAudioChanged(void)
{
  if (this->mediator->getState() == UIMediator::RUNNING) {
    if (this->playBack == nullptr) {
      if (this->ui.audioPanel->getEnabled()) {
        // Audio enabled, open it.
        (void) openAudio(this->ui.audioPanel->getSampleRate());
      }
    } else {
     if (this->ui.audioPanel->getEnabled()) {
       // Audio enabled, update parameters

       if (this->ui.audioPanel->getSampleRate() != this->audioSampleRate) {
         this->closeAudio();
         this->openAudio(this->ui.audioPanel->getSampleRate());

         // XXX: MEDIATE!!
         if (this->ui.audioPanel->getSampleRate() != this->audioSampleRate)
           this->ui.audioPanel->setSampleRate(this->audioSampleRate);
       }

       this->setAudioInspectorParams(
             this->audioSampleRate,
             this->ui.audioPanel->getCutOff(),
             this->ui.audioPanel->getVolume(),
             this->ui.audioPanel->getDemod() + 1);
     } else {
       // Disable audio
       closeAudio();
     }
    }
  }
}

void
Application::onAntennaChanged(QString name)
{
  if (this->mediator->getState() == UIMediator::RUNNING)
    this->analyzer->setAntenna(name.toStdString());
}