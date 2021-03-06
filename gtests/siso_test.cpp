/*!
 * \file
 * \brief SISO class test program
 * \author Bogdan Cristea
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 1995-2013  (see AUTHORS file for a list of contributors)
 *
 * This file is part of IT++ - a C++ library of mathematical, signal
 * processing, speech processing, and communications classes and functions.
 *
 * IT++ is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * IT++ is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with IT++.  If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "itpp/itcomm.h"
#include "gtest/gtest.h"

using namespace itpp;
using std::string;

static
void assert_vec_p(const vec &ref, const vec &act, int line)
{
  static const double tol = 1e-4;
  ASSERT_EQ(ref.length(), act.length()) << line;
  for (int n = 0; n < ref.length(); ++n) {
    ASSERT_NEAR(ref(n), act(n), tol) << line;
  }
}
#define assert_vec(ref, act) assert_vec_p(ref, act, __LINE__)

TEST(SISO, all)
{
    //general parameters
    string map_metric="maxlogMAP";
    ivec gen = "07 05";//octal form, feedback first
    int constraint_length = 3;
    int nb_errors_lim = 300;
    int nb_bits_lim = int(1e4);
    int perm_len = (1<<14);//total number of bits in a block (with tail)
    int nb_iter = 10;//number of iterations in the turbo decoder
    vec EbN0_dB = "0.0 0.5 1.0 1.5 2.0";
    double R = 1.0/3.0;//coding rate (non punctured PCCC)
    double Ec = 1.0;//coded bit energy

    //other parameters
    int nb_bits = perm_len-(constraint_length-1);//number of bits in a block (without tail)
    vec sigma2 = (0.5*Ec/R)*pow(inv_dB(EbN0_dB), -1.0);//N0/2
    double Lc;//scaling factor
    int nb_blocks;//number of blocks
    int nb_errors;
    ivec perm(perm_len);
    ivec inv_perm(perm_len);
    bvec bits(nb_bits);
    int cod_bits_len = perm_len*gen.length();
    bmat cod1_bits;//tail is added
    bvec tail;
    bvec cod2_input;
    bmat cod2_bits;
    int rec_len = int(1.0/R)*perm_len;
    bvec coded_bits(rec_len);
    vec rec(rec_len);
    vec dec1_intrinsic_coded(cod_bits_len);
    vec dec2_intrinsic_coded(cod_bits_len);
    vec apriori_data(perm_len);//a priori LLR for information bits
    vec extrinsic_coded(perm_len);
    vec extrinsic_data(perm_len);
    bvec rec_bits(perm_len);
    int snr_len = EbN0_dB.length();
    mat ber(nb_iter,snr_len);
    ber.zeros();
    register int en,n;

    //Recursive Systematic Convolutional Code
    Rec_Syst_Conv_Code cc;
    cc.set_generator_polynomials(gen, constraint_length);//initial state should be the zero state

    //BPSK modulator
    BPSK bpsk;

    //AWGN channel
    AWGN_Channel channel;

    //SISO modules
    SISO siso;
    siso.set_generators(gen, constraint_length);
    siso.set_map_metric(map_metric);

    //BER
    BERC berc;

    //Fix random generators
    RNG_reset(12345);

    //main loop
    for (en=0;en<snr_len;en++)
    {
        channel.set_noise(sigma2(en));
        Lc = -2/sigma2(en);//normalisation factor for intrinsic information (take into account the BPSK mapping)
        nb_errors = 0;
        nb_blocks = 0;
        while ((nb_errors<nb_errors_lim) && (nb_blocks*nb_bits<nb_bits_lim))//if at the last iteration the nb. of errors is inferior to lim, then process another block
        {
            //permutation
            perm = sort_index(randu(perm_len));
            //inverse permutation
            inv_perm = sort_index(perm);

            //bits generation
            bits = randb(nb_bits);

            //parallel concatenated convolutional code
            cc.encode_tail(bits, tail, cod1_bits);//tail is added here to information bits to close the trellis
            cod2_input = concat(bits, tail);
            cc.encode(cod2_input(perm), cod2_bits);
            for (n=0;n<perm_len;n++)//output with no puncturing
            {
                coded_bits(3*n) = cod2_input(n);//systematic output
                coded_bits(3*n+1) = cod1_bits(n,0);//first parity output
                coded_bits(3*n+2) = cod2_bits(n,0);//second parity output
            }

            //BPSK modulation (1->-1,0->+1) + AWGN channel
            rec = channel(bpsk.modulate_bits(coded_bits));

            //form input for SISO blocks
            for (n=0;n<perm_len;n++)
            {
                dec1_intrinsic_coded(2*n) = Lc*rec(3*n);
                dec1_intrinsic_coded(2*n+1) = Lc*rec(3*n+1);
                dec2_intrinsic_coded(2*n) = 0.0;//systematic output of the CC is already used in decoder1
                dec2_intrinsic_coded(2*n+1) = Lc*rec(3*n+2);
            }
            //turbo decoder
            apriori_data.zeros();//a priori LLR for information bits
            for (n=0;n<nb_iter;n++)
            {
                //first decoder
               siso.rsc(extrinsic_coded, extrinsic_data, dec1_intrinsic_coded, apriori_data, true);
                //interleave
                apriori_data = extrinsic_data(perm);
                //second decoder
                siso.rsc(extrinsic_coded, extrinsic_data, dec2_intrinsic_coded, apriori_data, false);

                //decision
                apriori_data += extrinsic_data;//a posteriori information
                rec_bits = bpsk.demodulate_bits(-apriori_data(inv_perm));//take into account the BPSK mapping
                //count errors
                berc.clear();
                berc.count(bits, rec_bits.left(nb_bits));
                ber(n,en) += berc.get_errorrate();

                //deinterleave for the next iteration
                apriori_data = extrinsic_data(inv_perm);
            }//end iterations
            nb_errors += int(berc.get_errors());//get number of errors at the last iteration
            nb_blocks++;
        }//end blocks (while loop)

        //compute BER over all tx blocks
        ber.set_col(en, ber.get_col(en)/nb_blocks);
    }

    // Results for max log MAP algorithm
    vec ref = "0.158039 0.110731 0.0770358 0.0445611 0.0235014";
    assert_vec(ref, ber.get_row(0));
    ref = "0.14168 0.0783177 0.0273471 0.00494445 0.00128189";
    assert_vec(ref, ber.get_row(1));
    ref = "0.141375 0.0565865 0.00817971 0.000305213 0";
    assert_vec(ref, ber.get_row(2));
    ref = "0.142412 0.0421194 0.000732511 0 0";
    assert_vec(ref, ber.get_row(3));
    ref = "0.144244 0.0282017 0.000183128 0 0";
    assert_vec(ref, ber.get_row(4));
    ref = "0.145587 0.0142229 0 0 0";
    assert_vec(ref, ber.get_row(5));
    ref = "0.148517 0.00714199 0 0 0";
    assert_vec(ref, ber.get_row(6));
    ref = "0.141619 0.00225858 0 0 0";
    assert_vec(ref, ber.get_row(7));
    ref = "0.149676 0.000549383 0 0 0";
    assert_vec(ref, ber.get_row(8));
    ref = "0.14461 0 0 0 0";
    assert_vec(ref, ber.get_row(9));
}
