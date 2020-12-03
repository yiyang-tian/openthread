/*
 *  Copyright (c) 2020, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements Backbone Router management.
 */

#include "bbr_manager.hpp"

#if OPENTHREAD_FTD && OPENTHREAD_CONFIG_BACKBONE_ROUTER_ENABLE

#include "common/code_utils.hpp"
#include "common/instance.hpp"
#include "common/locator-getters.hpp"
#include "common/logging.hpp"
#include "common/random.hpp"
#include "thread/mle_types.hpp"
#include "thread/thread_netif.hpp"
#include "thread/thread_tlvs.hpp"
#include "thread/uri_paths.hpp"

namespace ot {

namespace BackboneRouter {

Manager::Manager(Instance &aInstance)
    : InstanceLocator(aInstance)
    , mMulticastListenerRegistration(UriPath::kMlr, Manager::HandleMulticastListenerRegistration, this)
    , mDuaRegistration(UriPath::kDuaRegistrationRequest, Manager::HandleDuaRegistration, this)
    , mBackboneQuery(UriPath::kBackboneQuery, Manager::HandleBackboneQuery, this)
    , mBackboneAnswer(UriPath::kBackboneAnswer, Manager::HandleBackboneAnswer, this)
    , mNdProxyTable(aInstance)
    , mMulticastListenersTable(aInstance)
    , mTimer(aInstance, Manager::HandleTimer, this)
    , mBackboneTmfAgent(aInstance)
#if OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE
    , mDuaResponseStatus(ThreadStatusTlv::kDuaSuccess)
    , mMlrResponseStatus(ThreadStatusTlv::kMlrSuccess)
    , mDuaResponseIsSpecified(false)
    , mMlrResponseIsSpecified(false)
#endif
{
    mBackboneTmfAgent.AddResource(mBackboneQuery);
    mBackboneTmfAgent.AddResource(mBackboneAnswer);
}

void Manager::HandleNotifierEvents(Events aEvents)
{
    otError error;

    if (aEvents.Contains(kEventThreadBackboneRouterStateChanged))
    {
        if (Get<BackboneRouter::Local>().GetState() == OT_BACKBONE_ROUTER_STATE_DISABLED)
        {
            Get<Tmf::TmfAgent>().RemoveResource(mMulticastListenerRegistration);
            Get<Tmf::TmfAgent>().RemoveResource(mDuaRegistration);
            mTimer.Stop();
            mMulticastListenersTable.Clear();

            error = mBackboneTmfAgent.Stop();

            if (error != OT_ERROR_NONE)
            {
                otLogWarnBbr("Stop Backbone TMF agent: %s", otThreadErrorToString(error));
            }
            else
            {
                otLogInfoBbr("Stop Backbone TMF agent: %s", otThreadErrorToString(error));
            }
        }
        else
        {
            Get<Tmf::TmfAgent>().AddResource(mMulticastListenerRegistration);
            Get<Tmf::TmfAgent>().AddResource(mDuaRegistration);
            if (!mTimer.IsRunning())
            {
                mTimer.Start(kTimerInterval);
            }

            error = mBackboneTmfAgent.Start();

            if (error != OT_ERROR_NONE)
            {
                otLogCritBbr("Start Backbone TMF agent: %s", otThreadErrorToString(error));
            }
            else
            {
                otLogInfoBbr("Start Backbone TMF agent: %s", otThreadErrorToString(error));
            }
        }
    }
}

void Manager::HandleTimer(void)
{
    mMulticastListenersTable.Expire();
    mNdProxyTable.HandleTimer();

    mTimer.Start(kTimerInterval);
}

void Manager::HandleMulticastListenerRegistration(const Coap::Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    otError                    error     = OT_ERROR_NONE;
    bool                       isPrimary = Get<BackboneRouter::Local>().IsPrimary();
    ThreadStatusTlv::MlrStatus status    = ThreadStatusTlv::kMlrSuccess;
    BackboneRouterConfig       config;

    uint16_t     addressesOffset, addressesLength;
    Ip6::Address address;
    Ip6::Address addresses[kIPv6AddressesNumMax];
    uint8_t      failedAddressNum  = 0;
    uint8_t      successAddressNum = 0;
    TimeMilli    expireTime;
    uint32_t     timeout;
    uint16_t     commissionerSessionId;
    bool         hasCommissionerSessionIdTlv = false;
    bool         processTimeoutTlv           = false;

    VerifyOrExit(aMessage.IsConfirmablePostRequest(), error = OT_ERROR_PARSE);

#if OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE
    // Required by Test Specification 5.10.22 DUA-TC-26, only for certification purpose
    if (mMlrResponseIsSpecified)
    {
        mMlrResponseIsSpecified = false;
        ExitNow(status = mMlrResponseStatus);
    }
#endif

    VerifyOrExit(isPrimary, status = ThreadStatusTlv::kMlrBbrNotPrimary);

    // TODO: (MLR) send configured MLR response for Reference Device

    if (Tlv::Find<ThreadCommissionerSessionIdTlv>(aMessage, commissionerSessionId) == OT_ERROR_NONE)
    {
        const MeshCoP::CommissionerSessionIdTlv *commissionerSessionIdTlv =
            static_cast<const MeshCoP::CommissionerSessionIdTlv *>(
                Get<NetworkData::Leader>().GetCommissioningDataSubTlv(MeshCoP::Tlv::kCommissionerSessionId));

        VerifyOrExit(commissionerSessionIdTlv != nullptr &&
                         commissionerSessionIdTlv->GetCommissionerSessionId() == commissionerSessionId,
                     status = ThreadStatusTlv::kMlrGeneralFailure);

        hasCommissionerSessionIdTlv = true;
    }

    processTimeoutTlv =
        hasCommissionerSessionIdTlv && (Tlv::Find<ThreadTimeoutTlv>(aMessage, timeout) == OT_ERROR_NONE);

    VerifyOrExit(Tlv::FindTlvValueOffset(aMessage, IPv6AddressesTlv::kIPv6Addresses, addressesOffset,
                                         addressesLength) == OT_ERROR_NONE,
                 error = OT_ERROR_PARSE);
    VerifyOrExit(addressesLength % sizeof(Ip6::Address) == 0, status = ThreadStatusTlv::kMlrGeneralFailure);
    VerifyOrExit(addressesLength / sizeof(Ip6::Address) <= kIPv6AddressesNumMax,
                 status = ThreadStatusTlv::kMlrGeneralFailure);

    if (!processTimeoutTlv)
    {
        IgnoreError(Get<BackboneRouter::Leader>().GetConfig(config));

        timeout = config.mMlrTimeout;
    }
    else
    {
        VerifyOrExit(timeout < UINT32_MAX, status = ThreadStatusTlv::kMlrNoPersistent);

        if (timeout != 0)
        {
            uint32_t origTimeout = timeout;

            timeout = OT_MIN(timeout, static_cast<uint32_t>(Mle::kMlrTimeoutMax));
            timeout = OT_MAX(timeout, static_cast<uint32_t>(Mle::kMlrTimeoutMin));

            if (timeout != origTimeout)
            {
                otLogNoteBbr("MLR.req: MLR timeout is normalized from %u to %u", origTimeout, timeout);
            }
        }
    }

    expireTime = TimerMilli::GetNow() + TimeMilli::SecToMsec(timeout);

    for (uint16_t offset = 0; offset < addressesLength; offset += sizeof(Ip6::Address))
    {
        IgnoreError(aMessage.Read(addressesOffset + offset, address));

        if (timeout == 0)
        {
            mMulticastListenersTable.Remove(address);
        }
        else
        {
            bool failed = true;

            switch (mMulticastListenersTable.Add(address, expireTime))
            {
            case OT_ERROR_NONE:
                failed = false;
                break;
            case OT_ERROR_INVALID_ARGS:
                if (status == ThreadStatusTlv::kMlrSuccess)
                {
                    status = ThreadStatusTlv::kMlrInvalid;
                }
                break;
            case OT_ERROR_NO_BUFS:
                if (status == ThreadStatusTlv::kMlrSuccess)
                {
                    status = ThreadStatusTlv::kMlrNoResources;
                }
                break;
            default:
                OT_ASSERT(false);
            }

            if (failed)
            {
                addresses[failedAddressNum++] = address;
            }
            else
            {
                // Put successfully registered addresses at the end of `addresses`.
                addresses[kIPv6AddressesNumMax - (++successAddressNum)] = address;
            }
        }
    }

exit:
    if (error == OT_ERROR_NONE)
    {
        SendMulticastListenerRegistrationResponse(aMessage, aMessageInfo, status, addresses, failedAddressNum);
    }

    if (successAddressNum > 0)
    {
        SendBackboneMulticastListenerRegistration(&addresses[kIPv6AddressesNumMax - successAddressNum],
                                                  successAddressNum, timeout);
    }
}

void Manager::SendMulticastListenerRegistrationResponse(const Coap::Message &      aMessage,
                                                        const Ip6::MessageInfo &   aMessageInfo,
                                                        ThreadStatusTlv::MlrStatus aStatus,
                                                        Ip6::Address *             aFailedAddresses,
                                                        uint8_t                    aFailedAddressNum)
{
    otError        error   = OT_ERROR_NONE;
    Coap::Message *message = nullptr;

    VerifyOrExit((message = Get<Tmf::TmfAgent>().NewMessage()) != nullptr, error = OT_ERROR_NO_BUFS);

    SuccessOrExit(message->SetDefaultResponseHeader(aMessage));
    SuccessOrExit(message->SetPayloadMarker());

    SuccessOrExit(Tlv::Append<ThreadStatusTlv>(*message, aStatus));

    if (aFailedAddressNum > 0)
    {
        IPv6AddressesTlv addressesTlv;

        addressesTlv.Init();
        addressesTlv.SetLength(sizeof(Ip6::Address) * aFailedAddressNum);
        SuccessOrExit(error = message->Append(addressesTlv));

        for (uint8_t i = 0; i < aFailedAddressNum; i++)
        {
            SuccessOrExit(error = message->Append(aFailedAddresses[i]));
        }
    }

    SuccessOrExit(error = Get<Tmf::TmfAgent>().SendMessage(*message, aMessageInfo));

exit:
    FreeMessageOnError(message, error);
    otLogInfoBbr("Sent MLR.rsp (status=%d): %s", aStatus, otThreadErrorToString(error));
}

void Manager::SendBackboneMulticastListenerRegistration(const Ip6::Address *aAddresses,
                                                        uint8_t             aAddressNum,
                                                        uint32_t            aTimeout)
{
    otError           error   = OT_ERROR_NONE;
    Coap::Message *   message = nullptr;
    Ip6::MessageInfo  messageInfo;
    IPv6AddressesTlv  addressesTlv;
    BackboneTmfAgent &backboneTmf = Get<BackboneRouter::BackboneTmfAgent>();

    OT_ASSERT(aAddressNum >= kIPv6AddressesNumMin && aAddressNum <= kIPv6AddressesNumMax);

    VerifyOrExit((message = backboneTmf.NewMessage()) != nullptr, error = OT_ERROR_NO_BUFS);

    SuccessOrExit(error = message->InitAsNonConfirmablePost(UriPath::kBackboneMlr));
    SuccessOrExit(error = message->SetPayloadMarker());

    addressesTlv.Init();
    addressesTlv.SetLength(sizeof(Ip6::Address) * aAddressNum);
    SuccessOrExit(error = message->Append(addressesTlv));
    SuccessOrExit(error = message->AppendBytes(aAddresses, sizeof(Ip6::Address) * aAddressNum));

    SuccessOrExit(error = Tlv::Append<ThreadTimeoutTlv>(*message, aTimeout));

    messageInfo.SetPeerAddr(Get<BackboneRouter::Local>().GetAllNetworkBackboneRoutersAddress());
    messageInfo.SetPeerPort(BackboneRouter::kBackboneUdpPort); // TODO: Provide API for configuring Backbone COAP port.

    messageInfo.SetHopLimit(Mle::kDefaultBackboneHoplimit);
    messageInfo.SetIsHostInterface(true);

    SuccessOrExit(error = backboneTmf.SendMessage(*message, messageInfo));

exit:
    FreeMessageOnError(message, error);
    otLogInfoBbr("Sent BMLR.ntf: %s", otThreadErrorToString(error));
}

void Manager::HandleDuaRegistration(const Coap::Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    otError                    error     = OT_ERROR_NONE;
    ThreadStatusTlv::DuaStatus status    = ThreadStatusTlv::kDuaSuccess;
    bool                       isPrimary = Get<BackboneRouter::Local>().IsPrimary();
    uint32_t                   lastTransactionTime;
    bool                       hasLastTransactionTime;
    Ip6::Address               target;
    Ip6::InterfaceIdentifier   meshLocalIid;
#if OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE
    Coap::Code duaRespCoapCode = Coap::kCodeEmpty;
#endif

    VerifyOrExit(aMessageInfo.GetPeerAddr().GetIid().IsRoutingLocator(), error = OT_ERROR_DROP);
    VerifyOrExit(aMessage.IsConfirmablePostRequest(), error = OT_ERROR_PARSE);

    SuccessOrExit(error = Tlv::Find<ThreadTargetTlv>(aMessage, target));
    SuccessOrExit(error = Tlv::Find<ThreadMeshLocalEidTlv>(aMessage, meshLocalIid));

#if OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE
    if (mDuaResponseIsSpecified && (mDuaResponseTargetMlIid.IsUnspecified() || mDuaResponseTargetMlIid == meshLocalIid))
    {
        mDuaResponseIsSpecified = false;
        if (mDuaResponseStatus >= Coap::kCodeResponseMin)
        {
            duaRespCoapCode = static_cast<Coap::Code>(mDuaResponseStatus);
        }
        else
        {
            status = static_cast<ThreadStatusTlv::DuaStatus>(mDuaResponseStatus);
        }
        ExitNow();
    }
#endif

    VerifyOrExit(isPrimary, status = ThreadStatusTlv::kDuaNotPrimary);
    VerifyOrExit(Get<BackboneRouter::Leader>().IsDomainUnicast(target), status = ThreadStatusTlv::kDuaInvalid);

    hasLastTransactionTime = (Tlv::Find<ThreadLastTransactionTimeTlv>(aMessage, lastTransactionTime) == OT_ERROR_NONE);

    switch (mNdProxyTable.Register(target.GetIid(), meshLocalIid, aMessageInfo.GetPeerAddr().GetIid().GetLocator(),
                                   hasLastTransactionTime ? &lastTransactionTime : nullptr))
    {
    case OT_ERROR_NONE:
        // TODO: update its EID-to-RLOC Map Cache based on the pair {DUA, RLOC16-source} which is gleaned from the
        // DUA.req packet according to Thread Spec. 5.23.3.6.2
        break;
    case OT_ERROR_DUPLICATED:
        status = ThreadStatusTlv::kDuaDuplicate;
        break;
    case OT_ERROR_NO_BUFS:
        status = ThreadStatusTlv::kDuaNoResources;
        break;
    default:
        status = ThreadStatusTlv::kDuaGeneralFailure;
        break;
    }

exit:
    otLogInfoBbr("Received DUA.req on %s: %s", (isPrimary ? "PBBR" : "SBBR"), otThreadErrorToString(error));

    if (error == OT_ERROR_NONE)
    {
#if OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE
        if (duaRespCoapCode != Coap::kCodeEmpty)
        {
            IgnoreError(Get<Tmf::TmfAgent>().SendEmptyAck(aMessage, aMessageInfo, duaRespCoapCode));
        }
        else
#endif
        {
            SendDuaRegistrationResponse(aMessage, aMessageInfo, target, status);
        }
    }
}

void Manager::SendDuaRegistrationResponse(const Coap::Message &      aMessage,
                                          const Ip6::MessageInfo &   aMessageInfo,
                                          const Ip6::Address &       aTarget,
                                          ThreadStatusTlv::DuaStatus aStatus)
{
    otError        error   = OT_ERROR_NONE;
    Coap::Message *message = nullptr;

    VerifyOrExit((message = Get<Tmf::TmfAgent>().NewMessage()) != nullptr, error = OT_ERROR_NO_BUFS);

    SuccessOrExit(message->SetDefaultResponseHeader(aMessage));
    SuccessOrExit(message->SetPayloadMarker());

    SuccessOrExit(Tlv::Append<ThreadStatusTlv>(*message, aStatus));
    SuccessOrExit(Tlv::Append<ThreadTargetTlv>(*message, aTarget));

    SuccessOrExit(error = Get<Tmf::TmfAgent>().SendMessage(*message, aMessageInfo));

exit:
    FreeMessageOnError(message, error);
    otLogInfoBbr("Sent DUA.rsp for DUA %s, status %d %s", aTarget.ToString().AsCString(), aStatus,
                 otThreadErrorToString(error));
}

#if OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE
void Manager::ConfigNextDuaRegistrationResponse(const Ip6::InterfaceIdentifier *aMlIid, uint8_t aStatus)
{
    mDuaResponseIsSpecified = true;

    if (aMlIid)
    {
        mDuaResponseTargetMlIid = *aMlIid;
    }
    else
    {
        mDuaResponseTargetMlIid.Clear();
    }

    mDuaResponseStatus = aStatus;
}

void Manager::ConfigNextMulticastListenerRegistrationResponse(ThreadStatusTlv::MlrStatus aStatus)
{
    mMlrResponseIsSpecified = true;
    mMlrResponseStatus      = aStatus;
}
#endif

NdProxyTable &Manager::GetNdProxyTable(void)
{
    return mNdProxyTable;
}

bool Manager::ShouldForwardDuaToBackbone(const Ip6::Address &aAddress)
{
    bool              forwardToBackbone = false;
    Mac::ShortAddress rloc16;
    otError           error;

    VerifyOrExit(Get<Local>().IsPrimary());
    VerifyOrExit(Get<Leader>().IsDomainUnicast(aAddress));

    VerifyOrExit(!mNdProxyTable.IsRegistered(aAddress.GetIid()));

    error = Get<AddressResolver>().Resolve(aAddress, rloc16, /* aAllowAddressQuery */ false);
    VerifyOrExit(error != OT_ERROR_NONE || rloc16 == Get<Mle::MleRouter>().GetRloc16());

    // TODO: check if the DUA is an address of any Child?
    forwardToBackbone = true;

exit:
    return forwardToBackbone;
}

otError Manager::SendBackboneQuery(const Ip6::Address &aDua, uint16_t aRloc16)
{
    otError          error   = OT_ERROR_NONE;
    Coap::Message *  message = nullptr;
    Ip6::MessageInfo messageInfo;

    VerifyOrExit(Get<BackboneRouter::Local>().IsPrimary(), error = OT_ERROR_INVALID_STATE);

    VerifyOrExit((message = mBackboneTmfAgent.NewPriorityMessage()) != nullptr, error = OT_ERROR_NO_BUFS);

    SuccessOrExit(error = message->InitAsNonConfirmablePost(UriPath::kBackboneQuery));
    SuccessOrExit(error = message->SetPayloadMarker());

    SuccessOrExit(error = Tlv::Append<ThreadTargetTlv>(*message, aDua));

    if (aRloc16 != Mac::kShortAddrInvalid)
    {
        SuccessOrExit(error = Tlv::Append<ThreadRloc16Tlv>(*message, aRloc16));
    }

    messageInfo.SetPeerAddr(Get<BackboneRouter::Local>().GetAllDomainBackboneRoutersAddress());
    messageInfo.SetPeerPort(BackboneRouter::kBackboneUdpPort);

    messageInfo.SetHopLimit(Mle::kDefaultBackboneHoplimit);
    messageInfo.SetIsHostInterface(true);

    error = mBackboneTmfAgent.SendMessage(*message, messageInfo);

exit:
    otLogInfoBbr("SendBackboneQuery for %s (rloc16=%04x): %s", aDua.ToString().AsCString(), aRloc16,
                 otThreadErrorToString(error));
    FreeMessageOnError(message, error);
    return error;
}

void Manager::HandleBackboneQuery(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    static_cast<Manager *>(aContext)->HandleBackboneQuery(*static_cast<const Coap::Message *>(aMessage),
                                                          *static_cast<const Ip6::MessageInfo *>(aMessageInfo));
}

void Manager::HandleBackboneQuery(const Coap::Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    otError                error = OT_ERROR_NONE;
    Ip6::Address           dua;
    uint16_t               rloc16 = Mac::kShortAddrInvalid;
    NdProxyTable::NdProxy *ndProxy;

    VerifyOrExit(aMessageInfo.IsHostInterface(), error = OT_ERROR_DROP);

    VerifyOrExit(Get<Local>().IsPrimary(), error = OT_ERROR_INVALID_STATE);
    VerifyOrExit(aMessage.IsNonConfirmablePostRequest(), error = OT_ERROR_PARSE);

    SuccessOrExit(error = Tlv::Find<ThreadTargetTlv>(aMessage, dua));

    error = Tlv::Find<ThreadRloc16Tlv>(aMessage, rloc16);
    VerifyOrExit(error == OT_ERROR_NONE || error == OT_ERROR_NOT_FOUND);

    otLogInfoBbr("Received BB.qry from %s for %s (rloc16=%04x)", aMessageInfo.GetPeerAddr().ToString().AsCString(),
                 dua.ToString().AsCString(), rloc16);

    ndProxy = mNdProxyTable.ResolveDua(dua);
    VerifyOrExit(ndProxy != nullptr && !ndProxy->GetDadFlag(), error = OT_ERROR_NOT_FOUND);

    error = SendBackboneAnswer(aMessageInfo, dua, rloc16, *ndProxy);

exit:
    otLogInfoBbr("HandleBackboneQuery: %s", otThreadErrorToString(error));
}

void Manager::HandleBackboneAnswer(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    static_cast<Manager *>(aContext)->HandleBackboneAnswer(*static_cast<const Coap::Message *>(aMessage),
                                                           *static_cast<const Ip6::MessageInfo *>(aMessageInfo));
}

void Manager::HandleBackboneAnswer(const Coap::Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    otError                  error = OT_ERROR_NONE;
    bool                     proactive;
    Ip6::Address             dua;
    Ip6::InterfaceIdentifier meshLocalIid;
    uint16_t                 networkNameOffset, networkNameLength;
    uint32_t                 timeSinceLastTransaction;
    uint16_t                 srcRloc16 = Mac::kShortAddrInvalid;

    VerifyOrExit(aMessageInfo.IsHostInterface(), error = OT_ERROR_DROP);

    VerifyOrExit(Get<Local>().IsPrimary(), error = OT_ERROR_INVALID_STATE);
    VerifyOrExit(aMessage.IsPostRequest(), error = OT_ERROR_PARSE);

    proactive = !aMessage.IsConfirmable();

    SuccessOrExit(error = Tlv::Find<ThreadTargetTlv>(aMessage, dua));
    SuccessOrExit(error = Tlv::Find<ThreadMeshLocalEidTlv>(aMessage, meshLocalIid));
    SuccessOrExit(error = Tlv::Find<ThreadLastTransactionTimeTlv>(aMessage, timeSinceLastTransaction));

    SuccessOrExit(error =
                      Tlv::FindTlvValueOffset(aMessage, ThreadTlv::kNetworkName, networkNameOffset, networkNameLength));

    error = Tlv::Find<ThreadRloc16Tlv>(aMessage, srcRloc16);
    VerifyOrExit(error == OT_ERROR_NONE || error == OT_ERROR_NOT_FOUND);

    if (proactive)
    {
        HandleProactiveBackboneNotification(dua, meshLocalIid, timeSinceLastTransaction);
    }
    else if (srcRloc16 == Mac::kShortAddrInvalid)
    {
        HandleDadBackboneAnswer(dua, meshLocalIid);
    }
    else
    {
        HandleExtendedBackboneAnswer(dua, meshLocalIid, timeSinceLastTransaction, srcRloc16);
    }

    SuccessOrExit(error = mBackboneTmfAgent.SendEmptyAck(aMessage, aMessageInfo));

exit:
    otLogInfoBbr("HandleBackboneAnswer: %s", otThreadErrorToString(error));
}

otError Manager::SendProactiveBackboneNotification(const Ip6::Address &            aDua,
                                                   const Ip6::InterfaceIdentifier &aMeshLocalIid,
                                                   uint32_t                        aTimeSinceLastTransaction)
{
    return SendBackboneAnswer(Get<BackboneRouter::Local>().GetAllDomainBackboneRoutersAddress(),
                              BackboneRouter::kBackboneUdpPort, aDua, aMeshLocalIid, aTimeSinceLastTransaction,
                              Mac::kShortAddrInvalid);
}

otError Manager::SendBackboneAnswer(const Ip6::MessageInfo &     aQueryMessageInfo,
                                    const Ip6::Address &         aDua,
                                    uint16_t                     aSrcRloc16,
                                    const NdProxyTable::NdProxy &aNdProxy)
{
    return SendBackboneAnswer(aQueryMessageInfo.GetPeerAddr(), aQueryMessageInfo.GetPeerPort(), aDua,
                              aNdProxy.GetMeshLocalIid(), aNdProxy.GetTimeSinceLastTransaction(), aSrcRloc16);
}

otError Manager::SendBackboneAnswer(const Ip6::Address &            aDstAddr,
                                    uint16_t                        aDstPort,
                                    const Ip6::Address &            aDua,
                                    const Ip6::InterfaceIdentifier &aMeshLocalIid,
                                    uint32_t                        aTimeSinceLastTransaction,
                                    uint16_t                        aSrcRloc16)
{
    otError          error   = OT_ERROR_NONE;
    Coap::Message *  message = nullptr;
    Ip6::MessageInfo messageInfo;
    bool             proactive = aDstAddr.IsMulticast();

    VerifyOrExit((message = mBackboneTmfAgent.NewPriorityMessage()) != nullptr, error = OT_ERROR_NO_BUFS);

    SuccessOrExit(error = message->Init(proactive ? Coap::kTypeNonConfirmable : Coap::kTypeConfirmable, Coap::kCodePost,
                                        UriPath::kBackboneAnswer));
    SuccessOrExit(error = message->SetPayloadMarker());

    SuccessOrExit(error = Tlv::Append<ThreadTargetTlv>(*message, aDua));

    SuccessOrExit(error = Tlv::Append<ThreadMeshLocalEidTlv>(*message, aMeshLocalIid));

    SuccessOrExit(error = Tlv::Append<ThreadLastTransactionTimeTlv>(*message, aTimeSinceLastTransaction));

    {
        const Mac::NameData nameData = Get<Mac::Mac>().GetNetworkName().GetAsData();

        SuccessOrExit(error = Tlv::Append<ThreadNetworkNameTlv>(*message, nameData.GetBuffer(), nameData.GetLength()));
    }

    if (aSrcRloc16 != Mac::kShortAddrInvalid)
    {
        SuccessOrExit(Tlv::Append<ThreadRloc16Tlv>(*message, aSrcRloc16));
    }

    messageInfo.SetPeerAddr(aDstAddr);
    messageInfo.SetPeerPort(aDstPort);

    messageInfo.SetHopLimit(Mle::kDefaultBackboneHoplimit);
    messageInfo.SetIsHostInterface(true);

    error = mBackboneTmfAgent.SendMessage(*message, messageInfo);

exit:
    otLogInfoBbr("Send %s for %s (rloc16=%04x): %s", proactive ? "PRO_BB.ntf" : "BB.ans", aDua.ToString().AsCString(),
                 aSrcRloc16, otThreadErrorToString(error));

    FreeMessageOnError(message, error);
    return error;
}

void Manager::HandleDadBackboneAnswer(const Ip6::Address &aDua, const Ip6::InterfaceIdentifier &aMeshLocalIid)
{
    otError                error     = OT_ERROR_NONE;
    NdProxyTable::NdProxy *ndProxy   = mNdProxyTable.ResolveDua(aDua);
    bool                   duplicate = false;

    OT_UNUSED_VARIABLE(error);

    VerifyOrExit(ndProxy != nullptr, error = OT_ERROR_NOT_FOUND);

    duplicate = ndProxy->GetMeshLocalIid() != aMeshLocalIid;

    if (duplicate)
    {
        Ip6::Address dest;

        dest.SetToRoutingLocator(Get<Mle::MleRouter>().GetMeshLocalPrefix(), ndProxy->GetRloc16());
        Get<AddressResolver>().SendAddressError(aDua, aMeshLocalIid, &dest);
    }

    ot::BackboneRouter::NdProxyTable::NotifyDadComplete(*ndProxy, duplicate);

exit:
    otLogInfoBbr("HandleDadBackboneAnswer: %s, target=%s, mliid=%s, duplicate=%s", otThreadErrorToString(error),
                 aDua.ToString().AsCString(), aMeshLocalIid.ToString().AsCString(), duplicate ? "Y" : "N");
}

void Manager::HandleExtendedBackboneAnswer(const Ip6::Address &            aDua,
                                           const Ip6::InterfaceIdentifier &aMeshLocalIid,
                                           uint32_t                        aTimeSinceLastTransaction,
                                           uint16_t                        aSrcRloc16)
{
    Ip6::Address dest;

    dest.SetToRoutingLocator(Get<Mle::MleRouter>().GetMeshLocalPrefix(), aSrcRloc16);
    Get<AddressResolver>().SendAddressQueryResponse(aDua, aMeshLocalIid, &aTimeSinceLastTransaction, dest);

    otLogInfoBbr("HandleExtendedBackboneAnswer: target=%s, mliid=%s, LTT=%lds, rloc16=%04x",
                 aDua.ToString().AsCString(), aMeshLocalIid.ToString().AsCString(), aTimeSinceLastTransaction,
                 aSrcRloc16);
}

void Manager::HandleProactiveBackboneNotification(const Ip6::Address &            aDua,
                                                  const Ip6::InterfaceIdentifier &aMeshLocalIid,
                                                  uint32_t                        aTimeSinceLastTransaction)
{
    otError                error   = OT_ERROR_NONE;
    NdProxyTable::NdProxy *ndProxy = mNdProxyTable.ResolveDua(aDua);

    OT_UNUSED_VARIABLE(error);

    VerifyOrExit(ndProxy != nullptr, error = OT_ERROR_NOT_FOUND);

    if (ndProxy->GetMeshLocalIid() == aMeshLocalIid)
    {
        uint32_t localTimeSinceLastTransaction = ndProxy->GetTimeSinceLastTransaction();

        if (aTimeSinceLastTransaction <= localTimeSinceLastTransaction)
        {
            BackboneRouter::NdProxyTable::Erase(*ndProxy);
        }
        else
        {
            IgnoreError(SendProactiveBackboneNotification(aDua, ndProxy->GetMeshLocalIid(),
                                                          ndProxy->GetTimeSinceLastTransaction()));
        }
    }
    else
    {
        // Duplicated address detected, send ADDR_ERR.ntf to ff03::2 in the Thread network
        BackboneRouter::NdProxyTable::Erase(*ndProxy);
        Get<AddressResolver>().SendAddressError(aDua, aMeshLocalIid, nullptr);
    }

exit:
    otLogInfoBbr("HandleProactiveBackboneNotification: %s, target=%s, mliid=%s, LTT=%lds", otThreadErrorToString(error),
                 aDua.ToString().AsCString(), aMeshLocalIid.ToString().AsCString(), aTimeSinceLastTransaction);
}

} // namespace BackboneRouter

} // namespace ot

#endif // OPENTHREAD_FTD && OPENTHREAD_CONFIG_BACKBONE_ROUTER_ENABLE
