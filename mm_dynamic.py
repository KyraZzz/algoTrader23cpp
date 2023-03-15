# Copyright 2021 Optiver Asia Pacific Pty. Ltd.
#
# This file is part of Ready Trader Go.
#
#     Ready Trader Go is free software: you can redistribute it and/or
#     modify it under the terms of the GNU Affero General Public License
#     as published by the Free Software Foundation, either version 3 of
#     the License, or (at your option) any later version.
#
#     Ready Trader Go is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     GNU Affero General Public License for more details.
#
#     You should have received a copy of the GNU Affero General Public
#     License along with Ready Trader Go.  If not, see
#     <https://www.gnu.org/licenses/>.
import asyncio
import itertools
import numpy as np
import threading
import numpy as np

from typing import List

from ready_trader_go import BaseAutoTrader, Instrument, Lifespan, MAXIMUM_ASK, MINIMUM_BID, Side

MAX_LOT_SIZE = 20
POSITION_LIMIT = 100
TICK_SIZE_IN_CENTS = 100
ACTIVE_VOLUME_LIMIT = 200
ACTIVE_ORDERS_LIMIT = 10

MIN_BID_NEAREST_TICK = (
    MINIMUM_BID + TICK_SIZE_IN_CENTS) // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS
MAX_ASK_NEAREST_TICK = MAXIMUM_ASK // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS
THRESHOLD = 2e-3

INTERVAL = 5  # the optimisation interval in sequences, every second approx. 4 messages are received
ZLB = 5e-4  # the zero lower bound for no profit


class Order:
    def __init__(self, order_id, price, volume, FAK=False):
        self.order_id = order_id
        self.price = price
        self.volume = volume
        self.FAK = FAK
        """ status:
            - 0: new order
            - 1: partially filled
            - 2: fully filled
            - 3: FAK
        """

    def amend_volume(self, volume):
        self.volume = volume


class BidOrder(Order):
    def __init__(self, order_id, price, volume, FAK=False):
        super().__init__(order_id, price, volume, FAK)

    def __lt__(self, other):
        if isinstance(other, BidOrder):
            if self.price > other.price:
                return True
            elif self.price == other.price and self.order_id < other.order_id:
                return True
        return False


class AskOrder(Order):
    def __init__(self, order_id, price, volume, FAK=False):
        super().__init__(order_id, price, volume, FAK)

    def __lt__(self, other):
        if isinstance(other, AskOrder):
            if self.price < other.price:
                return True
            elif self.price == other.price and self.order_id < other.order_id:
                return True
        return False


class AutoTrader(BaseAutoTrader):
    """Example Auto-trader.

    When it starts this auto-trader places ten-lot bid and ask orders at the
    current best-bid and best-ask prices respectively. Thereafter, if it has
    a long position (it has bought more lots than it has sold) it reduces its
    bid and ask prices. Conversely, if it has a short position (it has sold
    more lots than it has bought) then it increases its bid and ask prices.
    """

    def __init__(self, loop: asyncio.AbstractEventLoop, team_name: str, secret: str):
        """Initialise a new instance of the AutoTrader class."""
        super().__init__(loop, team_name, secret)
        self.order_ids = itertools.count(1)
        self.bids = set()
        self.asks = set()
        self.ask_id = self.ask_price = self.bid_id = self.bid_price = self.position = self.active_volume = self.active_orders = 0
        self.pot_bid = self.pot_ask = 0
        self.top_bid_dic = dict()
        self.top_ask_dic = dict()
        self.order_map = dict()  # order_id -> order

        self.spreadA = []
        self.spreadB = []
        self.t_0 = 0  # time in units of sequence number : int
        self.NewThreshold = 2e-3
        self.Threshold = THRESHOLD  # I replaced all THRESHOLD

    def optimise(self, spreadA, spreadB):
        spreadA = np.array(spreadA)
        # spreadB = np.array(spreadB)

        # signs = np.sign(spreadA)
        # changes = np.diff(signs)
        # positive_groups = np.count_nonzero(changes == 2)
        # negative_groups = np.count_nonzero(changes == -2)
        # crossover_sum = positive_groups + negative_groups

        spreadA = spreadA[(spreadA > ZLB) & (spreadA < 1e-3)]
        # print(len(spreadA))
        # spreadB = np.sort(spreadB[spreadB>ZLB])

        # lenA = len(spreadA)
        # lenB = len(spreadB)

        # topA = int(lenA*0.1)
        # topB = int(lenB*0.1)

        # print(crossover_sum)
        # if crossover_sum > 5:
        #     topA = int(lenA*0.1)
        #     topB = int(lenB*0.1)
        # elif crossover_sum < 3:
        #     topA = int(lenA*0.8)
        #     topB = int(lenB*0.8)
        # else:
        #     topA = int(lenA*0.5)
        #     topB = int(lenB*0.5)

        # bound1 = spreadA[topA] if len(spreadA) > 0 else self.Threshold
        # bound2 = spreadB[topB] if len(spreadB) > 0 else self.Threshold

        # if len(spreadA) > 0 else self.Threshold
        bound1 = np.mean(spreadA) if len(spreadA) != 0 else self.Threshold
        # bound2 = spreadB[0] if len(spreadB) > 0 else self.Threshold

        self.NewThreshold = bound1  # np.min([bound1,bound2])

    def on_error_message(self, client_order_id: int, error_message: bytes) -> None:
        """Called when the exchange detects an error.

        If the error pertains to a particular order, then the client_order_id
        will identify that order, otherwise the client_order_id will be zero.
        """
        self.logger.warning("error with order %d: %s",
                            client_order_id, error_message.decode())
        if client_order_id != 0 and (client_order_id in self.bids or client_order_id in self.asks):
            self.on_order_status_message(client_order_id, 0, 0, 0)

    def on_hedge_filled_message(self, client_order_id: int, price: int, volume: int) -> None:
        """Called when one of your hedge orders is filled.

        The price is the average price at which the order was (partially) filled,
        which may be better than the order's limit price. The volume is
        the number of lots filled at that price.
        """
        self.logger.info("received hedge filled for order %d with average price %d and volume %d", client_order_id,
                         price, volume)

    def on_order_book_update_message(self, instrument: int, sequence_number: int, ask_prices: List[int],
                                     ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically to report the status of an order book.

        The sequence number can be used to detect missed or out-of-order
        messages. The five best available ask (i.e. sell) and bid (i.e. buy)
        prices are reported along with the volume available at each of those
        price levels.
        """
        '''The order book message provides entry and end signal for the strategy.

        The entry signal is triggered when the spread between the two instruments is larger than the THRESHOLD.
        More specifically, two 'realistic spreads' are monitored, which are (the best bid in the future - the 
        best ask in the etf) and (the best bid in the etf - the best ask in the future).
        If either of the two spreads is larger than the THRESHOLD*the lower of the two prices, then a profitable
        entry signal is triggered. We place two orders simultaneously to effectively short the spread.

        The end signal is triggered when the spread becomes 0. We place two orders to unwind and materialise our
        profits.

        This strategy is an improved version of the naive stat arb because it trades on all entry signals whenever
        our total position is within limit. And it closes our positions whenever the end signal is presented.
        '''

        self.logger.info("received order book for instrument %d with sequence number %d", instrument,
                         sequence_number)

        # current_time = sequence_number # current time in units of sequence number

        if instrument == Instrument.ETF:
            other = Instrument.FUTURE
            self.top_bid_dic[instrument] = [
                (price, bid_volumes[idx]) for idx, price in enumerate(bid_prices)]
            self.top_ask_dic[instrument] = [
                (price, ask_volumes[idx]) for idx, price in enumerate(ask_prices)]

            f_ask_p0 = self.top_ask_dic[other][0][0]
            f_bid_p0 = self.top_bid_dic[other][0][0]
            e_ask_p0 = self.top_ask_dic[instrument][0][0]
            e_bid_p0 = self.top_bid_dic[instrument][0][0]

            # entry signal
            if self.active_orders < ACTIVE_ORDERS_LIMIT and other in self.top_bid_dic.keys() and other in self.top_ask_dic.keys():
                if self.pot_bid < POSITION_LIMIT and f_bid_p0 - (e_bid_p0 + TICK_SIZE_IN_CENTS) >= self.Threshold * (e_bid_p0 + TICK_SIZE_IN_CENTS):
                    # insert bid in etf, (if successful) hit bid in future
                    volume = min(MAX_LOT_SIZE, POSITION_LIMIT -
                                 self.pot_bid)
                    if volume > 0:
                        self.bid_id = next(self.order_ids)
                        price = e_bid_p0 + TICK_SIZE_IN_CENTS
                        # send order
                        self.send_insert_order(
                            self.bid_id, Side.BUY, price, volume, Lifespan.G)
                        # update bookkeeping
                        self.bids.add(self.bid_id)
                        new_order = BidOrder(self.bid_id, price, volume)
                        self.order_map[self.bid_id] = new_order
                        self.pot_bid += volume
                        self.active_orders += 1
                if self.pot_ask > -POSITION_LIMIT and (e_ask_p0 - TICK_SIZE_IN_CENTS) - f_ask_p0 >= self.Threshold * f_ask_p0:
                    # insert ask in etf, (if successful) take offer in future
                    volume = min(MAX_LOT_SIZE, POSITION_LIMIT +
                                 self.pot_ask)
                    if volume > 0:
                        self.ask_id = next(self.order_ids)
                        price = e_ask_p0 - TICK_SIZE_IN_CENTS
                        # send order
                        self.send_insert_order(
                            self.ask_id, Side.SELL, price, volume, Lifespan.G)
                        # update bookkeeping
                        self.asks.add(self.ask_id)
                        new_order = AskOrder(self.ask_id, price, volume)
                        self.order_map[self.ask_id] = new_order
                        self.pot_ask -= volume
                        self.active_orders += 1

            # cancel orders signal - when the spread is less than the THRESHOLD on either side
            if f_bid_p0 - (e_bid_p0 + TICK_SIZE_IN_CENTS) < self.Threshold * (e_bid_p0 + TICK_SIZE_IN_CENTS):
                for bid in self.bids:
                    self.send_cancel_order(bid)
            if (e_ask_p0 - TICK_SIZE_IN_CENTS) - f_ask_p0 < self.Threshold * f_ask_p0:
                for ask in self.asks:
                    self.send_cancel_order(ask)

            # exit signal
            volume = abs(self.position)
            # when we have long etf and we need to sell it
            if self.position > 0 and e_bid_p0 > f_ask_p0 and self.active_orders < ACTIVE_ORDERS_LIMIT:
                self.ask_id = next(self.order_ids)
                # send order
                self.send_insert_order(
                    self.ask_id, Side.SELL, e_bid_p0, volume, Lifespan.F)
                # update bookkeeping
                self.asks.add(self.ask_id)
                new_order = AskOrder(self.ask_id, e_bid_p0, volume, True)
                self.order_map[self.ask_id] = new_order
                self.active_orders += 1
            # when we have short etf and we need to buy it
            elif self.position < 0 and f_bid_p0 > e_ask_p0 and self.active_orders < ACTIVE_ORDERS_LIMIT:
                self.bid_id = next(self.order_ids)
                # send order
                self.send_insert_order(
                    self.bid_id, Side.BUY, e_ask_p0, volume, Lifespan.F)
                # update bookkeeping
                self.bids.add(self.bid_id)
                new_order = BidOrder(self.bid_id, e_ask_p0, volume, True)
                self.order_map[self.bid_id] = new_order
                self.active_orders += 1

            spreadA = f_bid_p0 - (e_bid_p0 + TICK_SIZE_IN_CENTS)
            spreadA = spreadA / (e_bid_p0 + TICK_SIZE_IN_CENTS)
            spreadB = (e_ask_p0 - TICK_SIZE_IN_CENTS) - f_ask_p0
            spreadB = spreadB / (e_ask_p0 - TICK_SIZE_IN_CENTS)
            self.spreadA.append(spreadA)
            # self.spreadB.append(spreadB)
            self.spreadA.append(spreadB)

            # time_elapsed = current_time - self.t_0
            # if time_elapsed % INTERVAL == 0:
            # print(self.Threshold)
            spreadA = self.spreadA[-2*INTERVAL:]
            spreadB = self.spreadB[-INTERVAL:]
            x = threading.Thread(target=self.optimise, kwargs={
                                 'spreadA': spreadA, 'spreadB': spreadB})
            x.start()

            # if (time_elapsed - 1) % INTERVAL == 0:
            self.Threshold = self.NewThreshold
            # print(self.Threshold)

        elif instrument == Instrument.FUTURE:
            self.top_bid_dic[instrument] = [
                (price, bid_volumes[idx]) for idx, price in enumerate(bid_prices)]
            self.top_ask_dic[instrument] = [
                (price, ask_volumes[idx]) for idx, price in enumerate(ask_prices)]

    def on_order_filled_message(self, client_order_id: int, price: int, volume: int) -> None:
        """Called when one of your orders is filled, partially or fully.

        The price is the price at which the order was (partially) filled,
        which may be better than the order's limit price. The volume is
        the number of lots filled at that price.
        """
        self.logger.info("received order filled for order %d with price %d and volume %d", client_order_id,
                         price, volume)

        if client_order_id in self.bids:
            self.send_hedge_order(next(self.order_ids),
                                  Side.ASK, MIN_BID_NEAREST_TICK, volume)
            self.position += volume
            self.pot_ask += volume

        elif client_order_id in self.asks:
            self.send_hedge_order(next(self.order_ids),
                                  Side.BID, MAX_ASK_NEAREST_TICK, volume)
            self.position -= volume
            self.pot_bid -= volume

        order = self.order_map[client_order_id]
        # case 3: not FAK and fully filled
        if order.FAK == False and volume == order.volume:
            if client_order_id in self.bids:
                self.bids.discard(client_order_id)
            elif client_order_id in self.asks:
                self.asks.discard(client_order_id)
            self.active_orders -= 1

        # case 4: partially filled
        elif order.FAK == False and volume < order.volume:
            remaining_volume = order.volume - volume
            order.amend_volume(remaining_volume)

    def on_order_status_message(self, client_order_id: int, fill_volume: int, remaining_volume: int,
                                fees: int) -> None:
        """Called when the status of one of your orders changes.

        The fill_volume is the number of lots already traded, remaining_volume
        is the number of lots yet to be traded and fees is the total fees for
        this order. Remember that you pay fees for being a market taker, but
        you receive fees for being a market maker, so fees can be negative.

        If an order is cancelled its remaining volume will be zero.
        """
        self.logger.info("received order status for order %d with fill volume %d remaining %d and fees %d",
                         client_order_id, fill_volume, remaining_volume, fees)

        order = self.order_map[client_order_id]
        # case 1: self cancelled order
        if not order.FAK and remaining_volume == 0:
            # It could be either a bid or an ask
            if client_order_id in self.bids:
                self.pot_bid -= order.volume
            elif client_order_id in self.asks:
                self.pot_ask += order.volume
            self.bids.discard(client_order_id)
            self.asks.discard(client_order_id)
            self.active_orders -= 1

        # case 2: FAK
        elif order.FAK == True:
            if client_order_id in self.bids:
                self.pot_bid += fill_volume
                self.bids.discard(client_order_id)
            elif client_order_id in self.asks:
                self.pot_ask -= fill_volume
                self.asks.discard(client_order_id)
            self.active_orders -= 1

        # self.logger.info("[DEBUG] pot bid %d; pot ask: %d",
        #                  self.pot_bid, self.pot_ask)
        # self.logger.info("[DEBUG] position %d",
        #                  self.position)
        # self.logger.info(f"[DEBUG] self.bids: {self.bids}")
        # self.logger.info(f"[DEBUG] self.asks: {self.asks}")

    def on_trade_ticks_message(self, instrument: int, sequence_number: int, ask_prices: List[int],
                               ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically when there is trading activity on the market.

        The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
        has been trading activity are reported along with the aggregated volume
        traded at each of those price levels.

        If there are less than five prices on a side, then zeros will appear at
        the end of both the prices and volumes arrays.
        """
        self.logger.info("received trade ticks for instrument %d with sequence number %d", instrument,
                         sequence_number)
