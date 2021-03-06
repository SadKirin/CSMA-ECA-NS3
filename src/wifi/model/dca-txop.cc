/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2005 INRIA
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 *         Luis Sanabria-Russo <luis.sanabria@upf.edu>
 */

#include <math.h>

#include "ns3/assert.h"
#include "ns3/packet.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "ns3/uinteger.h"
#include "ns3/pointer.h"
#include "dca-txop.h"
#include "dcf-manager.h"
#include "mac-low.h"
#include "wifi-mac-queue.h"
#include "mac-tx-middle.h"
#include "wifi-mac-trailer.h"
#include "wifi-mac.h"
#include "random-stream.h"

#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT if (m_low != 0) { std::clog << "[mac=" << m_low->GetAddress () << "] "; }

#define MY_DEBUG(x) \
  NS_LOG_DEBUG (Simulator::Now () << " " << this << " " << x)

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("DcaTxop");

class DcaTxop::Dcf : public DcfState
{
public:
  Dcf (DcaTxop * txop)
    : m_txop (txop)
  {
  }

private:
  virtual void DoNotifyAccessGranted (void)
  {
    m_txop->NotifyAccessGranted ();
  }
  virtual void DoNotifyInternalCollision (void)
  {
    m_txop->NotifyInternalCollision ();
  }
  virtual void DoNotifyCollision (void)
  {
    m_txop->NotifyCollision ();
  }
  virtual void DoNotifyChannelSwitching (void)
  {
    m_txop->NotifyChannelSwitching ();
  }
  virtual void DoNotifySleep (void)
  {
    m_txop->NotifySleep ();
  }
  virtual void DoNotifyWakeUp (void)
  {
    m_txop->NotifyWakeUp ();
  }

  DcaTxop *m_txop;
};


/**
 * Listener for MacLow events. Forwards to DcaTxop.
 */
class DcaTxop::TransmissionListener : public MacLowTransmissionListener
{
public:
  /**
   * Create a TransmissionListener for the given DcaTxop.
   *
   * \param txop
   */
  TransmissionListener (DcaTxop * txop)
    : MacLowTransmissionListener (),
      m_txop (txop)
  {
  }

  virtual ~TransmissionListener ()
  {
  }

  virtual void GotCts (double snr, WifiMode txMode)
  {
    m_txop->GotCts (snr, txMode);
  }
  virtual void MissedCts (void)
  {
    m_txop->MissedCts ();
  }
  virtual void GotAck (double snr, WifiMode txMode)
  {
    m_txop->GotAck (snr, txMode);
  }
  virtual void MissedAck (void)
  {
    m_txop->MissedAck ();
  }
  virtual void StartNext (void)
  {
    m_txop->StartNext ();
  }
  virtual void Cancel (void)
  {
    m_txop->Cancel ();
  }
  virtual void EndTxNoAck (void)
  {
    m_txop->EndTxNoAck ();
  }

private:
  DcaTxop *m_txop;
};

NS_OBJECT_ENSURE_REGISTERED (DcaTxop);

TypeId
DcaTxop::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::DcaTxop")
    .SetParent<ns3::Dcf> ()
    .SetGroupName ("Wifi")
    .AddConstructor<DcaTxop> ()
    .AddAttribute ("Queue", "The WifiMacQueue object",
                   PointerValue (),
                   MakePointerAccessor (&DcaTxop::GetQueue),
                   MakePointerChecker<WifiMacQueue> ())
    .AddTraceSource ("TxFailures", "Incremented for each missed ACK",
                    MakeTraceSourceAccessor (&DcaTxop::m_failures),
                    "ns3::Traced::Value::Uint64Callback")
    .AddTraceSource ("TxSuccesses", "Incremented for each ACK",
                    MakeTraceSourceAccessor (&DcaTxop::m_successes),
                    "ns3::Traced::Value::Uint64Callback")
    .AddTraceSource ("TxAttempts", "Incremented for every access to DCF",
                    MakeTraceSourceAccessor (&DcaTxop::m_txAttempts),
                    "ns3::Traced::Value:Uint64Callback")
    .AddTraceSource ("BackoffCounter", "Changes in the assigned backoff",
                    MakeTraceSourceAccessor (&DcaTxop::m_boCounter),
                    "ns3::Traced::Value:Uint32Callback")
    .AddTraceSource ("Bitmap", "The bitmap state after every cycle",
                    MakeTraceSourceAccessor (&DcaTxop::m_ecaBitmap),
                    "ns3::Traced::Value::TracedEcaBitmap")
    .AddTraceSource ("SrReductionAttempts", "Number of reduction attempts",
                    MakeTraceSourceAccessor (&DcaTxop::m_scheduleReductionAttempts),
                    "ns3::Traced::Value::Uint32Callback")
    .AddTraceSource ("SrReductions", "Number of SR reductions",
                    MakeTraceSourceAccessor (&DcaTxop::m_scheduleReductions),
                    "ns3::Traced::Value:Uint32Callback")
    .AddTraceSource ("SrReductionFailed", "Times does not comply with SR criteria",
                    MakeTraceSourceAccessor (&DcaTxop::m_scheduleReductionFailed),
                    "ns3::Traced::Value:Uint32Callback")
  ;
  return tid;
}

DcaTxop::DcaTxop ()
  : m_manager (0),
    m_currentPacket (0),
    m_srActivationThreshold (0),
    m_srBeingFilled (false),
    m_srIterations (0),
    m_srReductionFactor (1),
    m_scheduleRecentlyReduced (false),
    m_srPreviousCw (0),
    m_failures (0),
    m_successes (0),
    m_txAttempts (0),
    m_boCounter (0xFFFFFFFF),
    m_ecaBitmap (false),
    m_scheduleReductions (0),
    m_scheduleReductionAttempts (0),
    m_scheduleReductionFailed (0)
{
  NS_LOG_FUNCTION (this);
  m_transmissionListener = new DcaTxop::TransmissionListener (this);
  m_dcf = new DcaTxop::Dcf (this);
  m_queue = CreateObject<WifiMacQueue> ();
  m_rng = new RealRandomStream ();
}

DcaTxop::~DcaTxop ()
{
  NS_LOG_FUNCTION (this);
}

void
DcaTxop::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_queue = 0;
  m_low = 0;
  m_stationManager = 0;
  delete m_transmissionListener;
  delete m_dcf;
  delete m_rng;
  m_transmissionListener = 0;
  m_dcf = 0;
  m_rng = 0;
  m_txMiddle = 0;
}

void
DcaTxop::SetManager (Ptr<DcfManager> manager)
{
  NS_LOG_FUNCTION (this << manager);
  m_manager = manager;
  m_manager->Add (m_dcf);
}

void DcaTxop::SetTxMiddle (MacTxMiddle *txMiddle)
{
  m_txMiddle = txMiddle;
}

void
DcaTxop::SetLow (Ptr<MacLow> low)
{
  NS_LOG_FUNCTION (this << low);
  m_low = low;
}

void
DcaTxop::SetWifiRemoteStationManager (Ptr<WifiRemoteStationManager> remoteManager)
{
  NS_LOG_FUNCTION (this << remoteManager);
  m_stationManager = remoteManager;
}

void
DcaTxop::SetTxOkCallback (TxOk callback)
{
  NS_LOG_FUNCTION (this << &callback);
  m_txOkCallback = callback;
}

void
DcaTxop::SetTxFailedCallback (TxFailed callback)
{
  NS_LOG_FUNCTION (this << &callback);
  m_txFailedCallback = callback;
}

Ptr<WifiMacQueue >
DcaTxop::GetQueue () const
{
  NS_LOG_FUNCTION (this);
  return m_queue;
}

void
DcaTxop::SetMinCw (uint32_t minCw)
{
  NS_LOG_FUNCTION (this << minCw);
  m_dcf->SetCwMin (minCw);
}

void
DcaTxop::SetMaxCw (uint32_t maxCw)
{
  NS_LOG_FUNCTION (this << maxCw);
  m_dcf->SetCwMax (maxCw);
}

void
DcaTxop::SetAifsn (uint32_t aifsn)
{
  NS_LOG_FUNCTION (this << aifsn);
  m_dcf->SetAifsn (aifsn);
}

uint32_t
DcaTxop::GetMinCw (void) const
{
  NS_LOG_FUNCTION (this);
  return m_dcf->GetCwMin ();
}

uint32_t
DcaTxop::GetMaxCw (void) const
{
  NS_LOG_FUNCTION (this);
  return m_dcf->GetCwMax ();
}

uint32_t
DcaTxop::GetAifsn (void) const
{
  NS_LOG_FUNCTION (this);
  return m_dcf->GetAifsn ();
}

void
DcaTxop::Queue (Ptr<const Packet> packet, const WifiMacHeader &hdr)
{
  NS_LOG_FUNCTION (this << packet << &hdr);
  WifiMacTrailer fcs;
  uint32_t fullPacketSize = hdr.GetSerializedSize () + packet->GetSize () + fcs.GetSerializedSize ();
  m_stationManager->PrepareForQueue (hdr.GetAddr1 (), &hdr,
                                     packet, fullPacketSize);
  m_queue->Enqueue (packet, hdr);
  StartAccessIfNeeded ();
}

int64_t
DcaTxop::AssignStreams (int64_t stream)
{
  NS_LOG_FUNCTION (this << stream);
  m_rng->AssignStreams (stream);
  return 1;
}

void
DcaTxop::RestartAccessIfNeeded (void)
{
  NS_LOG_FUNCTION (this);
  if ((m_currentPacket != 0
       || !m_queue->IsEmpty ())
      && !m_dcf->IsAccessRequested ())
    {
      m_manager->RequestAccess (m_dcf);
    }
}

void
DcaTxop::StartAccessIfNeeded (void)
{
  NS_LOG_FUNCTION (this);
  if (m_currentPacket == 0
      && !m_queue->IsEmpty ()
      && !m_dcf->IsAccessRequested ())
    {
      m_manager->RequestAccess (m_dcf);
    }
}

Ptr<MacLow>
DcaTxop::Low (void)
{
  NS_LOG_FUNCTION (this);
  return m_low;
}

bool
DcaTxop::NeedRts (Ptr<const Packet> packet, const WifiMacHeader *header)
{
  NS_LOG_FUNCTION (this << packet << header);
  return m_stationManager->NeedRts (header->GetAddr1 (), header,
                                    packet);
}

void
DcaTxop::DoInitialize ()
{
  NS_LOG_FUNCTION (this);
  m_dcf->ResetCw ();
  // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
  m_dcf->StartBackoffNow (tracedRandomFactory ());
  ns3::Dcf::DoInitialize ();
}

bool
DcaTxop::NeedRtsRetransmission (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->NeedRtsRetransmission (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                                  m_currentPacket);
}

bool
DcaTxop::NeedDataRetransmission (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->NeedDataRetransmission (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                                   m_currentPacket);
}

bool
DcaTxop::NeedFragmentation (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->NeedFragmentation (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                              m_currentPacket);
}

void
DcaTxop::NextFragment (void)
{
  NS_LOG_FUNCTION (this);
  m_fragmentNumber++;
}

uint32_t
DcaTxop::GetFragmentSize (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->GetFragmentSize (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                            m_currentPacket, m_fragmentNumber);
}

bool
DcaTxop::IsLastFragment (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->IsLastFragment (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                           m_currentPacket, m_fragmentNumber);
}

uint32_t
DcaTxop::GetNextFragmentSize (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->GetFragmentSize (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                            m_currentPacket, m_fragmentNumber + 1);
}

uint32_t
DcaTxop::GetFragmentOffset (void)
{
  NS_LOG_FUNCTION (this);
  return m_stationManager->GetFragmentOffset (m_currentHdr.GetAddr1 (), &m_currentHdr,
                                              m_currentPacket, m_fragmentNumber);
}

Ptr<Packet>
DcaTxop::GetFragmentPacket (WifiMacHeader *hdr)
{
  NS_LOG_FUNCTION (this << hdr);
  *hdr = m_currentHdr;
  hdr->SetFragmentNumber (m_fragmentNumber);
  uint32_t startOffset = GetFragmentOffset ();
  Ptr<Packet> fragment;
  if (IsLastFragment ())
    {
      hdr->SetNoMoreFragments ();
    }
  else
    {
      hdr->SetMoreFragments ();
    }
  fragment = m_currentPacket->CreateFragment (startOffset,
                                              GetFragmentSize ());
  return fragment;
}

bool
DcaTxop::NeedsAccess (void) const
{
  NS_LOG_FUNCTION (this);
  return !m_queue->IsEmpty () || m_currentPacket != 0;
}
void
DcaTxop::NotifyAccessGranted (void)
{
  NS_LOG_FUNCTION (this);
  m_txAttempts++;
  if (m_currentPacket == 0)
    {
      if (m_queue->IsEmpty ())
        {
          NS_LOG_DEBUG ("queue empty");
          return;
        }
      m_currentPacket = m_queue->Dequeue (&m_currentHdr);
      NS_ASSERT (m_currentPacket != 0);
      uint16_t sequence = m_txMiddle->GetNextSequenceNumberfor (&m_currentHdr);
      m_currentHdr.SetSequenceNumber (sequence);
      m_stationManager->UpdateFragmentationThreshold ();
      m_currentHdr.SetFragmentNumber (0);
      m_currentHdr.SetNoMoreFragments ();
      m_currentHdr.SetNoRetry ();
      m_fragmentNumber = 0;
      NS_LOG_DEBUG ("dequeued size=" << m_currentPacket->GetSize () <<
                    ", to=" << m_currentHdr.GetAddr1 () <<
                    ", seq=" << m_currentHdr.GetSequenceControl ());
    }
  MacLowTransmissionParameters params;
  params.DisableOverrideDurationId ();
  if (m_currentHdr.GetAddr1 ().IsGroup ())
    {
      params.DisableRts ();
      params.DisableAck ();
      params.DisableNextData ();
      Low ()->StartTransmission (m_currentPacket,
                                 &m_currentHdr,
                                 params,
                                 m_transmissionListener);
      NS_LOG_DEBUG ("tx broadcast");
    }
  else
    {
      params.EnableAck ();

      if (NeedFragmentation ())
        {
          WifiMacHeader hdr;
          Ptr<Packet> fragment = GetFragmentPacket (&hdr);
          if (NeedRts (fragment, &hdr))
            {
              params.EnableRts ();
            }
          else
            {
              params.DisableRts ();
            }
          if (IsLastFragment ())
            {
              NS_LOG_DEBUG ("fragmenting last fragment size=" << fragment->GetSize ());
              params.DisableNextData ();
            }
          else
            {
              NS_LOG_DEBUG ("fragmenting size=" << fragment->GetSize ());
              params.EnableNextData (GetNextFragmentSize ());
            }
          Low ()->StartTransmission (fragment, &hdr, params,
                                     m_transmissionListener);
        }
      else
        {
          if (NeedRts (m_currentPacket, &m_currentHdr))
            {
              params.EnableRts ();
              NS_LOG_DEBUG ("tx unicast rts");
            }
          else
            {
              params.DisableRts ();
              NS_LOG_DEBUG ("tx unicast: " << m_currentPacket->GetUid ());
            }
          params.DisableNextData ();
          Low ()->StartTransmission (m_currentPacket, &m_currentHdr,
                                     params, m_transmissionListener);
        }
    }
}

void
DcaTxop::NotifyInternalCollision (void)
{
  NS_LOG_FUNCTION (this);
  NotifyCollision ();
}

void
DcaTxop::NotifyCollision (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("collision");
  // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
  m_dcf->StartBackoffNow (tracedRandomFactory ());
  RestartAccessIfNeeded ();
}

void
DcaTxop::NotifyChannelSwitching (void)
{
  NS_LOG_FUNCTION (this);
  m_queue->Flush ();
  m_currentPacket = 0;
}

void
DcaTxop::NotifySleep (void)
{
  NS_LOG_FUNCTION (this);
  if (m_currentPacket != 0)
    {
      m_queue->PushFront (m_currentPacket, m_currentHdr);
      m_currentPacket = 0;
    }
}

void
DcaTxop::NotifyWakeUp (void)
{
  NS_LOG_FUNCTION (this);
  RestartAccessIfNeeded ();
}

void
DcaTxop::GotCts (double snr, WifiMode txMode)
{
  NS_LOG_FUNCTION (this << snr << txMode);
  NS_LOG_DEBUG ("got cts");
}

void
DcaTxop::MissedCts (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("missed cts");
  if (!NeedRtsRetransmission ())
    {
      NS_LOG_DEBUG ("Cts Fail");
      m_stationManager->ReportFinalRtsFailed (m_currentHdr.GetAddr1 (), &m_currentHdr);
      if (!m_txFailedCallback.IsNull ())
        {
          m_txFailedCallback (m_currentHdr);
        }
      //to reset the dcf.
      m_currentPacket = 0;
      m_dcf->ResetCw ();
    }
  else
    {
      m_dcf->UpdateFailedCw ();
    }
  // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
  m_dcf->StartBackoffNow (tracedRandomFactory ());
  RestartAccessIfNeeded ();
}


void
DcaTxop::GotAck (double snr, WifiMode txMode)
{
  m_successes++;
  NS_LOG_FUNCTION (this << snr << txMode);
  if (!NeedFragmentation ()
      || IsLastFragment ())
    {
      NS_LOG_DEBUG ("got ack. tx done.");
      AddConsecutiveSuccess();
      m_manager->ResetStickiness ();
      if (!m_txOkCallback.IsNull ())
        {
          m_txOkCallback (m_currentHdr);
        }

      /* we are not fragmenting or we are done fragmenting
       * so we can get rid of that packet now.
       */
      m_currentPacket = 0;

      /* Beginning  of CSMA/ECA */
      if (m_manager->GetEnvironmentForECA ())
        {
          NS_LOG_DEBUG ("***ECA=true");
          if (!m_manager->GetHysteresisForECA ())
            m_dcf->ResetCw ();

          if (m_manager->GetScheduleReset ())
            {
              if (m_scheduleRecentlyReduced == true)
                KeepScheduleReductionIfAny ();
              if (m_srActivationThreshold == 0) 
                SetScheduleResetActivationThreshold ( (m_dcf->GetCwMax ()+ 1) / (m_dcf->GetCw () + 1) );

              NS_LOG_DEBUG ("Schedule Reset. Sx #" << GetConsecutiveSuccesses ()
                << " activation thresh: " << GetScheduleResetActivationThreshold ());

              if (GetConsecutiveSuccesses () >= GetScheduleResetActivationThreshold ())
                {
                  if (!m_srBeingFilled)
                    {
                      NS_LOG_DEBUG ("Starting to fill the bitmap");
                      SetScheduleResetThreshold ();
                      uint32_t size = (m_dcf->GetCw () / 2) + 2; //number of elements in vector + 1
                      m_manager->StartNewEcaBitmap (size);
                      m_srBeingFilled = true;
                      m_manager->SetFillingTheBitmap ();
                      m_srIterations = GetConsecutiveSuccesses ();
                    }
                  else
                    {
                      if( (GetConsecutiveSuccesses () - m_srIterations) >= GetScheduleResetThreshold ())
                        {
                          NS_LOG_DEBUG ("Checking bitmap for Schedule Reset");
                          if(CanWeReduceTheSchedule ())
                            {
                              NS_LOG_DEBUG ("We can reduce the schedule");
                              ModifyCwAccordingToScheduleReduction ();
                            }
                          else
                            {
                              NS_LOG_DEBUG ("We cannot reduce the schedule");
                            }
                          m_srBeingFilled = false;
                          m_manager->SetNotFillingTheBitmap ();
                          ResetConsecutiveSuccess ();
                          m_srIterations = GetConsecutiveSuccesses ();
                        }
                    }
                } 
            }

          m_dcf->StartBackoffNow (deterministicBackoff (m_dcf->GetCw ()));
          NS_LOG_DEBUG ("Done processing success");
        }
      /* End of CSMA/ECA */
      else
        {
          m_dcf->ResetCw();
          // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
          m_dcf->StartBackoffNow (tracedRandomFactory ());
        }

      RestartAccessIfNeeded ();
    }
  else
    {
      NS_LOG_DEBUG ("got ack. tx not done, size=" << m_currentPacket->GetSize ());
    }
}

void
DcaTxop::MissedAck (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("missed ack");
  if (!NeedDataRetransmission ())
    {
      NS_LOG_DEBUG ("Ack Fail");
      m_stationManager->ReportFinalDataFailed (m_currentHdr.GetAddr1 (), &m_currentHdr);
      if (!m_txFailedCallback.IsNull ())
        {
          m_txFailedCallback (m_currentHdr);
        }
      //to reset the dcf.
      m_currentPacket = 0;
      m_dcf->ResetCw ();
    }
  else
    {
      MY_DEBUG ("Retransmit");
      m_currentHdr.SetRetry ();

      /* Reverting the Schedule Reset if any */
      if (m_manager->GetStickiness () == 0)
        {
          MY_DEBUG ("Node's stickiness: " << m_manager->GetStickiness ());
          if (m_manager->GetScheduleReset ())
            ResetSrMetrics ();
          ResetConsecutiveSuccess();
          m_dcf->UpdateFailedCw ();
          m_failures++;
          // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
          m_dcf->StartBackoffNow (tracedRandomFactory ());
        }
      else
        {
          NS_ASSERT (m_manager->GetStickiness () > 0);
          MY_DEBUG ("Reducing stickiness from: " << m_manager->GetStickiness ());
          m_manager->ReduceStickiness ();
          MY_DEBUG ("Assigning a deterministic backoff");
          m_dcf->StartBackoffNow (deterministicBackoff (m_dcf->GetCw ()));
        }
    }
  RestartAccessIfNeeded ();
}

void
DcaTxop::StartNext (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("start next packet fragment");
  /* this callback is used only for fragments. */
  NextFragment ();
  WifiMacHeader hdr;
  Ptr<Packet> fragment = GetFragmentPacket (&hdr);
  MacLowTransmissionParameters params;
  params.EnableAck ();
  params.DisableRts ();
  params.DisableOverrideDurationId ();
  if (IsLastFragment ())
    {
      params.DisableNextData ();
    }
  else
    {
      params.EnableNextData (GetNextFragmentSize ());
    }
  Low ()->StartTransmission (fragment, &hdr, params, m_transmissionListener);
}

void
DcaTxop::Cancel (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("transmission cancelled");
  /**
   * This happens in only one case: in an AP, you have two DcaTxop:
   *   - one is used exclusively for beacons and has a high priority.
   *   - the other is used for everything else and has a normal
   *     priority.
   *
   * If the normal queue tries to send a unicast data frame, but
   * if the tx fails (ack timeout), it starts a backoff. If the beacon
   * queue gets a tx oportunity during this backoff, it will trigger
   * a call to this Cancel function.
   *
   * Since we are already doing a backoff, we will get access to
   * the medium when we can, we have nothing to do here. We just
   * ignore the cancel event and wait until we are given again a
   * tx oportunity.
   *
   * Note that this is really non-trivial because each of these
   * frames is assigned a sequence number from the same sequence
   * counter (because this is a non-802.11e device) so, the scheme
   * described here fails to ensure in-order delivery of frames
   * at the receiving side. This, however, does not matter in
   * this case because we assume that the receiving side does not
   * update its <seq,ad> tupple for packets whose destination
   * address is a broadcast address.
   */
}

void
DcaTxop::EndTxNoAck (void)
{
  NS_LOG_FUNCTION (this);
  NS_LOG_DEBUG ("a transmission that did not require an ACK just finished");
  m_currentPacket = 0;
  
  if(m_manager->GetEnvironmentForECA ())
    {
      if (!m_manager->GetHysteresisForECA ())
        m_dcf->ResetCw ();
      m_dcf->StartBackoffNow (deterministicBackoff (m_dcf->GetCw ()));
    }
  else
    {
      m_dcf->ResetCw ();
      // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
      m_dcf->StartBackoffNow (tracedRandomFactory ());
    }
  StartAccessIfNeeded ();
}

//*
//* ECA stats and other parameters being adjusted */
//*

uint64_t
DcaTxop::GetSuccesses (void)
{
  return m_successes;
}

uint64_t
DcaTxop::GetFailures (void)
{
  return m_failures;
}

uint64_t
DcaTxop::GetTxAttempts (void)
{
  return m_txAttempts;
}

uint32_t
DcaTxop::GetAssignedBackoff (void)
{
  return m_boCounter;
}

uint32_t
DcaTxop::GetConsecutiveSuccesses (void)
{
  return m_consecutiveSuccess;
}

void
DcaTxop::AddConsecutiveSuccess (void)
{
  m_consecutiveSuccess++;
}

void
DcaTxop::ResetConsecutiveSuccess (void)
{
  m_consecutiveSuccess = 0;
}

void
DcaTxop::ResetStats (void)
{
  NS_LOG_DEBUG ("Resetting stats");

  m_failures = 0;
  m_successes = 0;
  m_txAttempts = 0;
  m_boCounter = 0xFFFFFFFF;
  m_ecaBitmap = false;
  m_consecutiveSuccess = 0;
  m_settingThreshold = false;
  m_scheduleResetThreshold = 0;
  m_scheduleResetMode = false;
  m_scheduleResetConservative = false;
  // m_dcf->StartBackoffNow (m_rng->GetNext (0, m_dcf->GetCw ()));
  m_dcf->StartBackoffNow (tracedRandomFactory ());
}

uint32_t
DcaTxop::deterministicBackoff (uint32_t cw)
{
  uint32_t tmp = ceil(cw / 2) + 1;
  if(!m_settingThreshold)
    {
      m_boCounter = 0xFFFFFFFF;
      m_boCounter = tmp;
    }
  m_settingThreshold = false;
  return tmp;
}

uint32_t
DcaTxop::tracedRandomFactory (void)
{
  m_boCounter = 0xFFFFFFFF;
  m_boCounter = m_rng->GetNext (0, m_dcf->GetCw ());
  // NS_LOG_DEBUG ("Setting random backoff using CSMA/ECA: " << m_boCounter);
  return m_boCounter;
}

bool
DcaTxop::GetScheduleResetMode (void)
{
  return m_scheduleResetMode;
}

void
DcaTxop::SetScheduleResetMode (void)
{
  m_scheduleResetMode = true;
}

bool
DcaTxop::CanWeReduceTheSchedule (void)
{
  bool canI = false;
  std::vector<bool> *bitmap = m_manager->GetBitmap ();
  NS_LOG_DEBUG ("Got the bitmap from DcfManager " << bitmap->size ());
  
  /* Updating the traced value */
  m_ecaBitmap = (std::vector<bool>*) 0;
  m_ecaBitmap = bitmap;

  /* Checking the possibility of a schedule reduction */
  uint32_t currentSize = bitmap->size ();
  if (!GetScheduleResetMode ())
    {
      /* That is, a Schedule Halving */
      /* Debugging the bitmap */
      NS_LOG_DEBUG ("Checking for Schedule Halving");
      uint32_t slot = 0; 
      for (std::vector<bool>::iterator i = bitmap->begin (); i != bitmap->end (); i++, slot++)
        {
          NS_LOG_DEBUG ("Bitmap position " << slot << " value: " << *i);
        }
      /* End of debug */

      if (!bitmap->at (currentSize/2 - 1))
        {
          NS_LOG_DEBUG ("A schedule halving is possible. Size: " << currentSize );
          canI = true;
          m_srReductionFactor = 2;
        }
    }
  else
    {
      /* Debugging the bitmap */
      NS_LOG_DEBUG ("Checking for Schedule Reset");
      uint32_t slot = 0; 
      for (std::vector<bool>::iterator i = bitmap->begin (); i != bitmap->end (); i++, slot++)
        {
          NS_LOG_DEBUG ("Bitmap position " << slot << " value: " << *i);
        }
      /* End of debug */
      

      uint32_t maxStage =  log2 ((m_dcf->GetCw () + 1) / (m_dcf->GetCwMin () + 1));
      if(maxStage > 1)
        {
          for (uint32_t i = 0; i <= maxStage; i++)
            {
              uint32_t position = pow (2,i) * m_dcf->GetCwMin () / 2;
              NS_ASSERT (position < currentSize);
              if (!bitmap->at (position))
              {
                NS_LOG_DEBUG ("A schedule reset is possible. Position: " << position );
                canI = true;
                m_srReductionFactor = (m_dcf->GetCw () + 1) / (pow (2,i) * m_dcf->GetCwMin () + 1);
                break;
              }
            }
        }
        else if (maxStage == 1)
        {
          canI = true;
          m_srReductionFactor = 1;
        }
    }

  /* Updating traced values */
  m_scheduleReductionAttempts++;
  if (canI == true)
    {
      m_scheduleReductions++;
    }
  else
    {
      m_scheduleReductionFailed++;
    }
  /* Done updating */
  return canI;
}

uint32_t
DcaTxop::GetScheduleResetThreshold (void)
{
  return m_scheduleResetThreshold;
}

void
DcaTxop::SetScheduleResetThreshold (void)
{
  m_settingThreshold = true;

  if(m_scheduleResetConservative)
    {
      m_scheduleResetThreshold = ceil (((m_dcf->GetCwMax () + 1) / 2) / 
        ( deterministicBackoff (m_dcf->GetCw ())));
    }
  else
    {
      m_scheduleResetThreshold = 1;
    }
}

void
DcaTxop::SetScheduleConservative (void)
{
  m_scheduleResetConservative = true;
}

void 
DcaTxop::SetScheduleResetActivationThreshold (uint32_t thresh)
{
  m_srActivationThreshold = thresh;
}

uint32_t
DcaTxop::GetScheduleResetActivationThreshold (void)
{
  return m_srActivationThreshold;
}

void
DcaTxop::ModifyCwAccordingToScheduleReduction (void)
{
  m_srPreviousCw =  m_dcf->GetCw ();
  uint32_t factor = GetScheduleReductionFactor ();
  NS_ASSERT (factor > 0);

  uint32_t reduced = ((m_dcf->GetCw () + 1) / factor) - 1;

  if(reduced > m_dcf->GetCwMin ())
    {
      m_dcf->SetCw (reduced);
    }
  else
    {
      m_dcf->SetCw (m_dcf->GetCwMin ());
    }

  NS_LOG_DEBUG ("Schedule Reset modified Cw from: " << m_srPreviousCw << " to: " << m_dcf->GetCw ());

  if (m_manager->UseDynamicStickiness ())
    m_manager->IncreaseStickiness ();
  m_scheduleRecentlyReduced = true;
}

uint32_t
DcaTxop::GetScheduleReductionFactor (void)
{
  return m_srReductionFactor;
}

void
DcaTxop::ResetSrMetrics (void)
{
  m_srBeingFilled = false;
  m_manager->SetNotFillingTheBitmap ();
  if (m_scheduleRecentlyReduced == true)
    {
      MY_DEBUG ("Resetting the chances made by Schedule Reset. Cw back to: " << m_srPreviousCw);
      m_dcf->SetCw (m_srPreviousCw);
    }
  m_scheduleRecentlyReduced = false;
}

void
DcaTxop::KeepScheduleReductionIfAny (void)
{
  m_scheduleRecentlyReduced = false;
}


} //namespace ns3
