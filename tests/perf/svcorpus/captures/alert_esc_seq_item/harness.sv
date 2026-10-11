// OpenTitan hw/dv/sv/alert_esc_agent/alert_esc_seq_item.sv, all constraint blocks
package p;
  typedef enum {
    AlertEscPingTrans,
    AlertEscSigTrans,
    AlertEscIntFail
  } alert_esc_trans_type_e;
  typedef enum {
    AlertPingReceived,
    AlertReceived,
    AlertAckReceived,
    AlertComplete,
    AlertAckComplete
  } alert_handshake_e;
  typedef enum {
    EscPingReceived,
    EscReceived,
    EscRespReceived,
    EscComplete,
    EscRespComplete,
    EscIntFail,
    EscRespHi,
    EscRespLo,
    EscRespPing0,
    EscRespPing1
  } esc_handshake_e;
  typedef enum bit [1:0] {
    NoAlertBeforeAfterIntFail  = 'b00,
    HasAlertBeforeIntFailOnly  = 'b01,
    HasAlertAfterIntFailOnly   = 'b10,
    HasAlertBeforeAfterIntFail = 'b11
  } alert_sig_int_err_e;
endpackage
import p::*;
class alert_esc_seq_item;
  rand bit s_alert_send;
  rand bit s_alert_ping_rsp;
  rand bit r_alert_rsp;
  rand bit r_alert_ping_send;
  rand bit r_esc_rsp;
  rand bit int_err;
  rand bit standalone_int_err;
  rand bit ping_timeout;
  rand alert_sig_int_err_e alert_int_err_type;
  rand alert_esc_trans_type_e alert_esc_type;
  rand alert_handshake_e      alert_handshake_sta;
  rand esc_handshake_e        esc_handshake_sta;
  rand int                    sig_cycle_cnt;
  rand int unsigned ping_delay;
  rand int unsigned ack_delay;
  rand int unsigned ack_stable;
  rand int unsigned alert_delay;
  rand int unsigned int_err_cyc;

  constraint delay_c {
    soft ping_delay  dist {0 :/ 5, [1:10] :/ 5};
    soft ack_delay   dist {0 :/ 5, [1:10] :/ 5};
    soft alert_delay dist {0 :/ 5, [1:10] :/ 5};
    soft ack_stable  dist {1 :/ 5, [2:10] :/ 5};
    soft int_err_cyc dist {1 :/ 5, [2:10] :/ 5};
  }
  constraint alert_esc_mode_c {
    r_esc_rsp == 1 -> (!s_alert_send && !r_alert_rsp && !r_alert_ping_send && !s_alert_ping_rsp);
    (s_alert_send || r_alert_rsp || r_alert_ping_send || s_alert_ping_rsp) -> !r_esc_rsp;
  }
  constraint sig_int_err_c {
    alert_int_err_type == NoAlertBeforeAfterIntFail;
  }
endclass
module top;
  initial begin
    alert_esc_seq_item t = new;
    for (int i = 0; i < 20; i++) begin
      if (!t.randomize()) $display("FAIL");
      $display("%0d pd=%0d ad=%0d ald=%0d as=%0d iec=%0d esc=%0d sa=%0d ra=%0d rp=%0d sp=%0d ty=%0d ah=%0d eh=%0d", i, t.ping_delay, t.ack_delay, t.alert_delay, t.ack_stable, t.int_err_cyc, t.r_esc_rsp, t.s_alert_send, t.r_alert_rsp, t.r_alert_ping_send, t.s_alert_ping_rsp, t.alert_esc_type, t.alert_handshake_sta, t.esc_handshake_sta);
    end
    $finish;
  end
endmodule
