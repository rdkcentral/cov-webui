#include <iostream>
#include <memory>
#include <string>
#include <cstdlib>
#include <cstring>

namespace SampleTestA {

    void testAudioModuleA() {
        std::shared_ptr<alexaClientSDK::defaultClient::DefaultClient> client = alexaClientSDK::defaultClient::DefaultClient::create(
            deviceInfo,
            customerDataManager,
            m_externalMusicProviderMediaPlayersMap,
            m_externalMusicProviderSpeakersMap,
            m_adapterToCreateFuncMap,
            m_speakMediaPlayer,
            std::move(audioMediaPlayerFactory),
            m_alertsMediaPlayer,
            m_notificationsMediaPlayer,
            m_bluetoothMediaPlayer,
            m_ringtoneMediaPlayer,
            m_systemSoundMediaPlayer,
            speakerMediaInterfaces->speaker,
            audioSpeakers,
            alertsMediaInterfaces->speaker,
            notificationMediaInterfaces->speaker,
            bluetoothMediaInterfaces->speaker,
            ringtoneMediaInterfaces->speaker,
            systemSoundMediaInterfaces->speaker,
            {},
            nullptr,
            audioFactory,
            authDelegate,
            std::move(alertStorage),
            std::move(messageStorage),
            std::move(notificationsStorage),
            std::move(deviceSettingsStorage),
            nullptr,
            miscStorage,
            { userInterfaceManager },
            { userInterfaceManager },
            std::move(internetConnectionMonitor),
            displayCardsSupported,
            m_capabilitiesDelegate,
            contextManager,
            transportFactory,
            avsGatewayManager,
            localeAssetsManager,
            {},
            nullptr,
            firmwareVersion,
            true,
            nullptr,
            nullptr,
            metricRecorder,
            nullptr,
            nullptr,
            std::make_shared<alexaClientSDK::sampleApp::ExternalCapabilitiesBuilder>(deviceInfo),
            std::make_shared<alexaClientSDK::capabilityAgents::speakerManager::DefaultChannelVolumeFactory>(),
            true,
            std::make_shared<alexaClientSDK::acl::MessageRouterFactory>(),
            nullptr,
            tapToTalkAudioProvider);
    }

    bool testTransportAndGateway() {
        // Context
        auto contextManager = manufactory->get<std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface>>();
        if (!contextManager) {
            return false;
        }

        auto avsGatewayManagerStorage = avsGatewayManager::storage::AVSGatewayManagerStorage::create(miscStorage);
        if (!avsGatewayManagerStorage) {
            return false;
        }
        auto avsGatewayManager = avsGatewayManager::AVSGatewayManager::create(
            std::move(avsGatewayManagerStorage), customerDataManager, config, authDelegate);
        if (!avsGatewayManager) {
            return false;
        }

        auto synchronizeStateSenderFactory = synchronizeStateSender::SynchronizeStateSenderFactory::create(contextManager);
        if (!synchronizeStateSenderFactory) {
            return false;
        }

        std::vector<std::shared_ptr<avsCommon::sdkInterfaces::PostConnectOperationProviderInterface>> providers;
        providers.push_back(synchronizeStateSenderFactory);
        providers.push_back(avsGatewayManager);
        providers.push_back(m_capabilitiesDelegate);

        /*
         * Create a factory for creating objects that handle tasks that need to be performed right after establishing
         * a connection to AVS.
         */
        auto postConnectSequencerFactory = acl::PostConnectSequencerFactory::create(providers);

        /*
         * Create a factory to create objects that establish a connection with AVS.
         */
        auto transportFactory = std::make_shared<acl::HTTP2TransportFactory>(
            std::make_shared<avsCommon::utils::libcurlUtils::LibcurlHTTP2ConnectionFactory>(),
            postConnectSequencerFactory,
            nullptr,
            nullptr);
        if (!transportFactory) {
            return false;
        }

        return true;
    }

}