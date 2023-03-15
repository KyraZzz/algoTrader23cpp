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
#ifndef CPPREADY_TRADER_GO_AUTOTRADER_H
#define CPPREADY_TRADER_GO_AUTOTRADER_H

#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <iostream>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/baseautotrader.h>
#include <ready_trader_go/types.h>

// Order class
class Order
{
public:
    Order(int order_id, int price, int volume, bool FAK = false)
        : order_id(order_id), price(price), volume(volume), FAK(FAK) {}

public:
    // Default constructor
    Order()
    {
        // Set default values for member variables
        order_id = 0;
        price = 0;
        volume = 0;
        FAK = false;
    }
    int order_id;
    int price;
    int volume;
    bool FAK;
    void amend_volume(int new_volume)
    {
        volume = new_volume;
    }
};

// BidOrder class
class BidOrder : public Order
{
public:
    BidOrder(int order_id, int price, int volume, bool FAK = false)
        : Order(order_id, price, volume, FAK) {}
    bool operator<(const BidOrder &other) const
    {
        if (price > other.price)
        {
            return true;
        }
        else if (price == other.price && order_id < other.order_id)
        {
            return true;
        }
        return false;
    }
};

// AskOrder class
class AskOrder : public Order
{
public:
    AskOrder(int order_id, int price, int volume, bool FAK = false)
        : Order(order_id, price, volume, FAK) {}
    bool operator<(const AskOrder &other) const
    {
        if (price < other.price)
        {
            return true;
        }
        else if (price == other.price && order_id < other.order_id)
        {
            return true;
        }
        return false;
    }
};

class AutoTrader : public ReadyTraderGo::BaseAutoTrader
{
public:
    explicit AutoTrader(boost::asio::io_context &context);

    // Called when the execution connection is lost.
    void DisconnectHandler() override;

    // Called when the matching engine detects an error.
    // If the error pertains to a particular order, then the client_order_id
    // will identify that order, otherwise the client_order_id will be zero.
    void ErrorMessageHandler(unsigned long clientOrderId,
                             const std::string &errorMessage) override;

    // Called when one of your hedge orders is filled, partially or fully.
    //
    // The price is the average price at which the order was (partially) filled,
    // which may be better than the order's limit price. The volume is
    // the number of lots filled at that price.
    //
    // If the order was unsuccessful, both the price and volume will be zero.
    void HedgeFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called periodically to report the status of an order book.
    // The sequence number can be used to detect missed or out-of-order
    // messages. The five best available ask (i.e. sell) and bid (i.e. buy)
    // prices are reported along with the volume available at each of those
    // price levels.
    void OrderBookMessageHandler(ReadyTraderGo::Instrument instrument,
                                 unsigned long sequenceNumber,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &askPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &askVolumes,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &bidPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &bidVolumes) override;

    // Called when one of your orders is filled, partially or fully.
    void OrderFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called when the status of one of your orders changes.
    // The fill volume is the number of lots already traded, remaining volume
    // is the number of lots yet to be traded and fees is the total fees paid
    // or received for this order.
    // Remaining volume will be set to zero if the order is cancelled.
    void OrderStatusMessageHandler(unsigned long clientOrderId,
                                   unsigned long fillVolume,
                                   unsigned long remainingVolume,
                                   signed long fees) override;

    // Called periodically when there is trading activity on the market.
    // The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
    // has been trading activity are reported along with the aggregated volume
    // traded at each of those price levels.
    // If there are less than five prices on a side, then zeros will appear at
    // the end of both the prices and volumes arrays.
    void TradeTicksMessageHandler(ReadyTraderGo::Instrument instrument,
                                  unsigned long sequenceNumber,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &askPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &askVolumes,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &bidPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT> &bidVolumes) override;

private:
    unsigned long mNextMessageId = 1;
    unsigned long mAskId = 0;
    unsigned long mAskPrice = 0;
    unsigned long mBidId = 0;
    unsigned long mBidPrice = 0;
    signed long mPosition = 0;
    signed long mActiveOrders = 0;
    signed long mPotBid = 0;
    signed long mPotAsk = 0;
    std::unordered_set<unsigned long> mAsks;
    std::unordered_set<unsigned long> mBids;
    std::unordered_map<ReadyTraderGo::Instrument, std::array<std::pair<signed long, signed long>, 5>> topBidDic;
    std::unordered_map<ReadyTraderGo::Instrument, std::array<std::pair<signed long, signed long>, 5>> topAskDic;
    std::unordered_map<signed long, Order> orderMap;
};

#endif // CPPREADY_TRADER_GO_AUTOTRADER_H
