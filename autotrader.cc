// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr signed long MAX_LOT_SIZE = 25;
constexpr signed long POSITION_LIMIT = 100;
constexpr signed long TICK_SIZE_IN_CENTS = 100;
constexpr signed long MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr signed long MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr float THRESHOLD = 5e-4;
// constexpr signed long ACTIVE_VOLUME_LIMIT = 200;
constexpr signed long ACTIVE_ORDERS_LIMIT = 10;

AutoTrader::AutoTrader(boost::asio::io_context &context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string &errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAsks.count(clientOrderId) == 1) || (mBids.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";
}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    if (instrument == Instrument::ETF)
    {
        for (int i = 0; i < sizeof(bidPrices) / sizeof(signed long); i++)
        {
            topBidDic[instrument][i] = std::make_pair(bidPrices[i], bidVolumes[i]);
        }
        for (int i = 0; i < sizeof(askPrices) / sizeof(signed long); i++)
        {
            topAskDic[instrument][i] = std::make_pair(askPrices[i], askVolumes[i]);
        }
        signed long f_ask_p0 = topAskDic[Instrument::FUTURE][0].first;
        signed long f_bid_p0 = topBidDic[Instrument::FUTURE][0].first;
        signed long e_ask_p0 = topAskDic[Instrument::ETF][0].first;
        signed long e_bid_p0 = topBidDic[Instrument::ETF][0].first;

        // entry signal
        if (mActiveOrders < ACTIVE_ORDERS_LIMIT && topBidDic.count(Instrument::FUTURE) > 0 && topAskDic.count(Instrument::FUTURE) > 0)
        {
            if (mPotBid < POSITION_LIMIT && f_bid_p0 - (e_bid_p0 + TICK_SIZE_IN_CENTS) >= THRESHOLD * (e_bid_p0 + TICK_SIZE_IN_CENTS))
            {
                signed long volume = std::min(MAX_LOT_SIZE, POSITION_LIMIT - mPotBid);
                if (volume > 0)
                {
                    mBidId = mNextMessageId++;
                    signed long price = e_bid_p0 + TICK_SIZE_IN_CENTS;
                    SendInsertOrder(mBidId, Side::BUY, price, volume, Lifespan::GOOD_FOR_DAY);
                    mBids.insert(mBidId);
                    BidOrder new_order(mBidId, price, volume, false);
                    orderMap[mBidId] = new_order;
                    mPotBid += volume;
                    mActiveOrders += 1;
                }
            }
            if (mPotAsk > -POSITION_LIMIT && (e_ask_p0 - TICK_SIZE_IN_CENTS) - f_ask_p0 >= THRESHOLD * f_ask_p0)
            {
                signed long volume = std::min(MAX_LOT_SIZE, POSITION_LIMIT + mPotAsk);
                if (volume > 0)
                {
                    mAskId = mNextMessageId++;
                    signed long price = e_ask_p0 - TICK_SIZE_IN_CENTS;
                    SendInsertOrder(mAskId, Side::SELL, price, volume, Lifespan::GOOD_FOR_DAY);
                    mAsks.insert(mAskId);
                    AskOrder new_order(mAskId, price, volume, false);
                    orderMap[mAskId] = new_order;
                    mPotAsk -= volume;
                    mActiveOrders += 1;
                }
            }
        }

        // cancel orders signal
        if (f_bid_p0 - (e_bid_p0 + TICK_SIZE_IN_CENTS) < THRESHOLD * (e_bid_p0 + TICK_SIZE_IN_CENTS))
        {
            for (signed long bid : mBids)
            {
                SendCancelOrder(bid);
            }
        }
        if ((e_ask_p0 - TICK_SIZE_IN_CENTS) - f_ask_p0 < THRESHOLD * f_ask_p0)
        {
            for (signed long ask : mAsks)
            {
                SendCancelOrder(ask);
            }
        }

        // Exit signal
        signed long volume = std::abs(mPosition);

        // When we have long ETF and we need to sell it
        if (mPosition > 0 && e_bid_p0 > f_ask_p0 && mActiveOrders < ACTIVE_ORDERS_LIMIT)
        {
            mAskId = mNextMessageId++; // Assuming you have a function to get the next order ID
            // Send order
            SendInsertOrder(mAskId, Side::SELL, e_bid_p0, volume, Lifespan::FILL_AND_KILL); // Assuming Side and Lifespan are defined
            // Update bookkeeping
            mAsks.insert(mAskId);
            AskOrder new_order(mAskId, e_bid_p0, volume, true);
            orderMap[mAskId] = new_order;
            mActiveOrders += 1;
        }
        // When we have short ETF and we need to buy it
        else if (mPosition < 0 && f_bid_p0 > e_ask_p0 && mActiveOrders < ACTIVE_ORDERS_LIMIT)
        {
            mBidId = mNextMessageId++; // Assuming you have a function to get the next order ID
            // Send order
            SendInsertOrder(mBidId, Side::BUY, e_ask_p0, volume, Lifespan::FILL_AND_KILL); // Assuming Side and Lifespan are defined
            // Update bookkeeping
            mBids.insert(mBidId);
            BidOrder new_order(mBidId, e_ask_p0, volume, true);
            orderMap[mBidId] = new_order;
            mActiveOrders += 1;
        }
    }

    if (instrument == Instrument::FUTURE)
    {
        for (int i = 0; i < sizeof(bidPrices) / sizeof(signed long); i++)
        {
            topBidDic[instrument][i] = std::make_pair(bidPrices[i], bidVolumes[i]);
        }
        for (int i = 0; i < sizeof(askPrices) / sizeof(signed long); i++)
        {
            topAskDic[instrument][i] = std::make_pair(askPrices[i], askVolumes[i]);
        }
    }
}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents";

    if (mBids.find(clientOrderId) != mBids.end())
    {
        SendHedgeOrder(mNextMessageId++, Side::SELL, MIN_BID_NEARST_TICK, volume); // Assuming getNextOrderId() and Side are defined
        mPosition += volume;
        mPotAsk += volume;
    }
    else if (mAsks.find(clientOrderId) != mAsks.end())
    {
        SendHedgeOrder(mNextMessageId++, Side::BUY, MAX_ASK_NEAREST_TICK, volume); // Assuming getNextOrderId() and Side are defined
        mPosition -= volume;
        mPotBid -= volume;
    }

    Order &order = orderMap[clientOrderId];
    // Case 3: not FAK and fully filled
    if (order.FAK == false && volume == order.volume)
    {
        if (mBids.find(clientOrderId) != mBids.end())
        {
            mBids.erase(clientOrderId);
        }
        else if (mAsks.find(clientOrderId) != mAsks.end())
        {
            mAsks.erase(clientOrderId);
        }
        mActiveOrders -= 1;
    }
    // Case 4: partially filled
    else if (order.FAK == false && volume < order.volume)
    {
        signed long remainingVolume = order.volume - volume;
        order.amend_volume(remainingVolume);
    }
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    Order &order = orderMap[clientOrderId];
    // case 1: self cancelled order
    if (!order.FAK && remainingVolume == 0)
    {
        // It could be either a bid or an ask
        if (mBids.find(clientOrderId) != mBids.end())
        {
            mPotBid -= order.volume;
        }
        else if (mAsks.find(clientOrderId) != mAsks.end())
        {
            mPotAsk += order.volume;
        }
        mBids.erase(clientOrderId);
        mAsks.erase(clientOrderId);
        mActiveOrders--;

        // case 2: FAK
    }
    else if (order.FAK)
    {
        if (mBids.find(clientOrderId) != mBids.end())
        {
            mPotBid += fillVolume;
            mBids.erase(clientOrderId);
        }
        else if (mAsks.find(clientOrderId) != mAsks.end())
        {
            mPotAsk -= fillVolume;
            mAsks.erase(clientOrderId);
        }
        mActiveOrders--;
    }
}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];
}
