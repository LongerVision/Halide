// This file defines a generator for a first order IIR low pass filter
// for a 2D image.

#include "Halide.h"

using namespace Halide;
using namespace Halide::BoundaryConditions;

class BoxBlur : public Generator<BoxBlur> {
public:
    Input<Buffer<uint8_t>> input{"input", 2};
    Input<int> radius{"radius"};
    Input<int> out_width{"out_width"}, out_height{"out_height"};
    Output<Buffer<uint8_t>> intermediate{"intermediate", 2};
    Output<Buffer<uint8_t>> output{"output", 2};

    Var x{"x"}, y{"y"};

    Func blur_cols_transpose(Func in, Expr height, bool first_pass) {
        Expr diameter = 2 * radius + 1;
        Expr inv_diameter = 1.f / diameter;
        RDom r_init(-radius, diameter);
        RDom ry(1, height - 1);

        Func wrap("wrap");
        wrap(x, y) = in(x, y);

        // Transpose the input
        Func transpose("transpose");
        transpose(x, y) = wrap(y, x);

        // Blur in y
        std::vector<Func> blurs, dithered;
        for (Type t : {UInt(16), UInt(32)}) {

            const bool should_dither = true;

            auto normalize = [&](Expr num) {
                if (!should_dither) {
                    // Exact integer division using tricks in the spirit of Hacker's Delight.
                    Type wide = t.with_bits(t.bits() * 2);
                    Expr shift = 31 - count_leading_zeros(diameter);
                    Expr wide_one = cast(wide, 1);
                    Expr mul = (wide_one << (t.bits() + shift + 1)) / diameter - (1 << t.bits()) + 1;
                    num += diameter / 2;
                    Expr e = cast(wide, num);
                    e *= mul;
                    e = e >> t.bits();
                    e = cast(t, e);
                    e += (num - e) / 2;
                    e = e >> shift;
                    e = cast<uint8_t>(e);
                    return e;
                } else {
                    return cast<uint8_t>(floor(num * inv_diameter + random_float()));
                }
            };

            Func blur{"blur_" + std::to_string(t.bits())};
            blur(x, y) = undef(t);
            blur(x, 0) = cast(t, 0);
            blur(x, 0) += cast(t, transpose(x, r_init));

            // Derivative of a box
            Expr v =
                (cast(Int(16), transpose(x, ry + radius)) -
                 transpose(x, ry - radius - 1));

            // It's a 9-bit signed integer. Sign-extend then treat it as a
            // uint16/32 with wrap-around. We know that the result can't
            // possibly be negative in the end, so this gives us an extra
            // bit of headroom while accumulating.
            v = cast(t, cast(Int(t.bits()), v));

            blur(x, ry) = blur(x, ry - 1) + v;

            blurs.push_back(blur);

            Func dither;
            dither(x, y) = normalize(blur(x, y));
            dithered.push_back(dither);
        }

        const int vec = get_target().natural_vector_size<uint16_t>();

        Func out;
        out(x, y) = select(diameter < 256, dithered[0](x, y), dithered[1](x, y));

        // Schedule.  Split the transpose into tiles of
        // rows. Parallelize strips.
        Var xo, yo, xi, yi, xoo;
        out
            .compute_root()
            .split(x, xoo, xo, vec * 2)
            .split(xo, xo, xi, vec)
            .reorder(xi, y, xo, xoo)
            .vectorize(xi)
            .parallel(xoo);

        // Run the filter on each row of tiles (which corresponds to a strip of
        // columns in the input).
        for (int i = 0; i < 2; i++) {
            Func blur = blurs[i];
            Func dither = dithered[i];
            blur.compute_at(out, xo)
                .store_in(MemoryType::Stack);

            blur.update(0).vectorize(x);
            blur.update(1).vectorize(x);

            // Vectorize computations within the strips.
            blur.update(2)
                .reorder(x, ry)
                .vectorize(x);

            dither
                .compute_at(out, y)
                .vectorize(x);
        }

        transpose
            .compute_at(out, xo)
            .store_in(MemoryType::Stack)
            .split(y, yo, yi, vec)
            .unroll(x)
            .vectorize(yi);

        wrap
            .compute_at(transpose, yo)
            .store_in(MemoryType::Register)
            .vectorize(x)
            .unroll(y);

        out.specialize(diameter < 256);

        return out;
    }

    void generate() {
        // First, blur the columns of the input.
        Func blury_T = blur_cols_transpose(input, out_width, true);

        intermediate = blury_T;

        // Blur the columns again (the rows of the original).
        Func blur = blur_cols_transpose(blury_T, out_height, false);

        output = blur;
    }
};

HALIDE_REGISTER_GENERATOR(BoxBlur, box_blur)

class BoxBlurLog : public Generator<BoxBlurLog> {
public:
    Input<Buffer<uint8_t>> input{"input", 2};
    Input<int> radius{"radius"};
    Output<Buffer<uint8_t>> output{"output", 2};

    void generate() {
        Expr diameter = cast<uint32_t>(2 * radius + 1);
        Var x, y;
        Func clamped = BoundaryConditions::repeat_edge(input);

        Func in16;
        in16(x, y) = cast<uint16_t>(clamped(x, y));

        // Assume diameter < 256
        std::vector<Func> horiz_blurs, vert_blurs;
        Expr result = in16(x, y - radius);
        Expr offset = -radius + 1;
        Func prev = in16;
        for (int i = 0; i < 8; i++) {
            Func next("blur_y_" + std::to_string(1 << i));
            next(x, y) = prev(x, y) + prev(x, y + (1 << i));
            prev = next;
            vert_blurs.push_back(next);

            Expr use_this = ((diameter >> (i + 1)) & 1) == 1;
            result += select(use_this, next(x, y + offset), 0);
            offset += select(use_this, (1 << i), 0);
        }

        Func blur_y;
        blur_y(x, y) = cast<uint8_t>(clamp((result + diameter / 2) / diameter, 0, 255));

        horiz_blurs.push_back(blur_y);

        result = blur_y(x - radius, y);
        offset = -radius + 1;
        prev = blur_y;
        for (int i = 0; i < 8; i++) {
            Func next("blur_x_" + std::to_string(1 << i));
            next(x, y) = prev(x, y) + prev(x + (1 << i), y);
            prev = next;
            horiz_blurs.push_back(next);

            Expr use_this = ((diameter >> (i + 1)) & 1) == 1;
            result += select(use_this, next(x + offset, y), 0);
            offset += select(use_this, (1 << i), 0);
        }

        output(x, y) = cast<uint8_t>(clamp((result + diameter / 2) / diameter, 0, 255));

        Var yi, yo;
        output
            .vectorize(x, natural_vector_size<uint8_t>())
            .split(y, yo, yi, 64, TailStrategy::GuardWithIf)
            .parallel(yo);

        clamped.compute_at(output, yo).vectorize(_0, natural_vector_size<uint8_t>());

        for (Func b : vert_blurs) {
            b
                .compute_at(output, yo)
                .store_in(MemoryType::Stack)
                .vectorize(x, natural_vector_size<uint16_t>());
        }

        for (Func b : horiz_blurs) {
            b
                .compute_at(output, yi)
                .store_in(MemoryType::Stack)
                .vectorize(x, natural_vector_size<uint16_t>());
        }
    }
};

HALIDE_REGISTER_GENERATOR(BoxBlurLog, box_blur_log)

// This generator is only responsible for producing N scanlines of output
class BoxBlurIncremental : public Generator<BoxBlurIncremental> {
public:
    const int N = 8;

    // The 8-bit input
    Input<Buffer<uint8_t>> input{"input", 2};

    // The input, already blurred in y and sum-scanned in x, for the N scanlines above the
    // one we're responsible for producing. Stored transposed.
    Input<Buffer<uint32_t>> prev_blur_y{"prev_blur_y", 2};
    Input<bool> prev_blur_y_valid{"prev_blur_y_valid"};
    Input<int> radius{"radius"};
    Input<int> width{"width"};

    Output<Buffer<uint32_t>> blur_y{"blur_y", 2};
    Output<Buffer<uint8_t>> output{"output", 2};

    void generate() {
        Expr diameter = cast<uint32_t>(2 * radius + 1);

        // First update prev_blur_y
        Func delta{"delta"};
        Var x{"x"}, y{"y"};
        delta(x, y) = cast<int16_t>(input(x, y + diameter - 1)) - input(x, y - 1);

        // Sum scan it
        RDom r_scan(1, N - 1);
        delta(x, r_scan) += delta(x, r_scan - 1);

        Func transpose{"transpose"};
        transpose(x, y) = delta(y, x);

        // The input, blurred in y and sum-scanned in x at this output
        blur_y(x, y) = undef<uint32_t>();
        blur_y(x, -1) = cast<uint32_t>(0);

        RDom r(0, width + 2 * radius);
        r.where(prev_blur_y_valid);
        blur_y(x, r) = ((prev_blur_y(N - 1, r) - prev_blur_y(N - 1, r - 1)) +
                        cast<uint32_t>(cast<int32_t>(transpose(x, r))) +
                        blur_y(x, r - 1));

        Func blur_y_direct{"blur_y_direct"};
        RDom rb(0, cast<int>(diameter));
        blur_y_direct(x, y) = cast<uint32_t>(0);
        blur_y_direct(x, 0) += cast<uint32_t>(input(x, rb));
        blur_y_direct(x, r_scan) =
            (blur_y_direct(x, r_scan - 1) +
             cast<uint32_t>(cast<int32_t>((cast<int16_t>(input(x, r_scan + diameter - 1)) -
                                           input(x, r_scan - 1)))));

        Func blur_y_direct_transpose{"blur_y_direct_transpose"};
        blur_y_direct_transpose(x, y) = blur_y_direct(y, x);

        RDom r_init(0, width + 2 * radius);
        r_init.where(!prev_blur_y_valid);
        blur_y(x, r_init) = blur_y(x, r_init - 1) + blur_y_direct_transpose(x, r_init);

        Func dithered{"dithered"};
        Expr result_32 = blur_y(x, y + diameter - 1) - blur_y(x, y - 1);

        bool should_dither = false;
        auto normalize = [&](Expr num) {
            Expr den = diameter * diameter;
            if (!should_dither) {
                /*
                  Exact integer version. For 32-bit ints it's actually slower than just
                  converting to float.

                Type t = num.type();
                Type wide = t.with_bits(t.bits() * 2);
                Expr shift = 31 - count_leading_zeros(den);
                Expr wide_one = cast(wide, 1);
                num += den / 2;
                Expr e = cast(wide, num);
                Expr mul = (wide_one << (t.bits() + shift + 1)) / den - (wide_one << t.bits()) + wide_one;
                e *= mul;
                e = e >> t.bits();
                e = cast(t, e);
                e += (num - e) / 2;
                e = e >> shift;
                return cast<uint8_t>(e);
                */
                return cast<uint8_t>(round(num * (1.0f / den)));
            } else {
                return cast<uint8_t>(floor(num * (1.0f / den) + random_float()));
            }
        };

        dithered(x, y) = normalize(result_32);

        output(x, y) = dithered(y, x);

        Var xi, yi;
        RVar ry, ryi;
        blur_y
            .dim(0)
            .set_bounds(0, N);
        blur_y
            .compute_root()
            .bound(x, 0, N);
        blur_y
            .update(0)
            .vectorize(x);
        blur_y
            .update(1)
            .split(r, ry, ryi, N)
            .reorder(x, ryi, ry)
            .vectorize(x);
        blur_y
            .update(2)
            .split(r_init, ry, ryi, N)
            .reorder(x, ryi, ry)
            .vectorize(x)
            .unroll(ryi);

        delta
            .compute_at(blur_y, ry)
            .vectorize(x, N)
            .unroll(y);
        delta
            .update()
            .vectorize(x, N)
            .unroll(r_scan);

        transpose
            .compute_at(blur_y, ry)
            .bound_extent(y, N)
            .vectorize(y)
            .unroll(x);

        blur_y_direct
            .compute_at(blur_y, ry)
            .vectorize(x)
            .unroll(y);
        blur_y_direct
            .update(0)
            .vectorize(x);
        blur_y_direct
            .update(1)
            .unroll(r_scan)
            .vectorize(x);
        blur_y_direct_transpose
            .compute_at(blur_y, ry)
            .bound_extent(y, N)
            .vectorize(y)
            .unroll(x);

        output
            .dim(1)
            .set_bounds(0, N);
        output
            .compute_root()
            .bound(y, 0, N)
            .split(x, x, xi, N)
            .reorder(xi, y, x)
            .vectorize(xi)
            .unroll(y);
        dithered
            .compute_at(output, x)
            .vectorize(x)
            .unroll(y);
        dithered.in()
            .compute_at(output, x)
            .reorder_storage(y, x)
            .vectorize(x)
            .unroll(y);
    }
};

HALIDE_REGISTER_GENERATOR(BoxBlurIncremental, box_blur_incremental)

class BoxBlurPyramid : public Generator<BoxBlurPyramid> {
public:
    Input<Buffer<uint8_t>> input{"input", 2};
    Input<int> diameter{"diameter"};
    Input<int> width{"width"};
    Output<Buffer<uint8_t>> output{"output", 2};

    void generate() {
        Var x("x"), y("y"), ty("ty"), tx("tx"), yo("yo"), yi("yi");

        const int N = 8;

        const int vec = 16;

        // We use slightly different algorithms as a function of the
        // max diameter supported. They get muxed together at the end.

        // For large radius, we'll downsample in y by a factor proportionate
        // to sqrt(diameter) ahead of time. We pick sqrt(diameter)
        // because it equalizes the number of samples taken inside the
        // low res and high res images, giving the best computational
        // complexity.
        Expr down_factor = clamp(cast<int>(ceil(sqrt(diameter))), N, 256);
        RDom r_down(0, down_factor);
        Func down_y("down_y");
        down_y(x, y) += cast<uint16_t>(input(x, y * down_factor + r_down));

        const int max_diameter_direct_blur_x = 6;    // Tuned empirically.
        const int max_diameter_16_bit_blur_x = 16;   // Must be <= 16 or we'll get overflow. TODO: Figure out why this makes things slower?!
        const int max_diameter_direct_blur_y = 80;   // Tuned empirically
        const int max_diameter_16_bit_blur_y = 256;  // Must be <= 256 or we'll get overflow
        const int max_diameter_supported = 32768;

        std::vector<int> max_diameters{max_diameter_direct_blur_x,
                                       max_diameter_16_bit_blur_x,
                                       max_diameter_direct_blur_y,
                                       max_diameter_16_bit_blur_y,
                                       max_diameter_supported};
        std::vector<Expr> results, conditions;
        for (int max_diameter : max_diameters) {

            Func blur_y_init("blur_y_init");
            Func blur_y("blur_y");

            // Slice the footprint of the vertical blur into three pieces.
            Expr fine_start_1 = ty * N;
            Expr fine_end_2 = ty * N + diameter;
            Expr coarse_start = (fine_start_1 - 1) / down_factor + 1;
            Expr coarse_end = fine_end_2 / down_factor;
            Expr fine_end_1 = coarse_start * down_factor;
            Expr fine_start_2 = coarse_end * down_factor;

            Expr coarse_pieces = coarse_end - coarse_start;
            Expr fine_pieces_1 = fine_end_1 - fine_start_1;
            Expr fine_pieces_2 = fine_end_2 - fine_start_2;

            // An empirically-tuned threshold for when it starts making
            // sense to use the downsampled-in-y input to boost the
            // initial blur.
            const bool use_down_y = max_diameter > max_diameter_direct_blur_y;

            RDom ry_init_fine_1(0, down_factor - 1);
            ry_init_fine_1.where(ry_init_fine_1 < fine_pieces_1);

            RDom ry_init_coarse(0, diameter / down_factor);
            ry_init_coarse.where(ry_init_coarse < coarse_pieces);

            RDom ry_init_fine_2(0, down_factor - 1);
            ry_init_fine_2.where(ry_init_fine_2 < fine_pieces_2);

            RDom ry_init_full(0, diameter);

            int bits = (max_diameter <= max_diameter_16_bit_blur_y) ? 16 : 32;
            Type t = UInt(bits);

            blur_y_init(x, ty) = cast(t, 0);
            if (use_down_y) {
                blur_y_init(x, ty) += cast(t, input(x, fine_start_1 + ry_init_fine_1));
                blur_y_init(x, ty) += cast(t, down_y(x, coarse_start + ry_init_coarse));
                blur_y_init(x, ty) += cast(t, input(x, fine_start_2 + ry_init_fine_2));
            } else {
                blur_y_init(x, ty) += cast(t, input(x, ty * N + ry_init_full));
            }

            // Compute the other in-between scanlines by incrementally
            // updating that one in a sliding window.
            RDom ry_scan(0, N - 1);
            blur_y(x, ty, y) = undef(t);
            blur_y(x, ty, 0) = blur_y_init(x, ty);
            blur_y(x, ty, ry_scan + 1) =
                (blur_y(x, ty, ry_scan) +
                 cast(t, (cast<int16_t>(input(x, ty * N + ry_scan + diameter)) -
                          input(x, ty * N + ry_scan))));

            // For large diameter, we do the blur in x using the regular
            // sliding window approach.

            const bool use_blur_x_direct = max_diameter <= max_diameter_direct_blur_x;

            bits = (max_diameter <= max_diameter_16_bit_blur_x) ? 16 : 32;
            t = UInt(bits);

            Func integrate_x("integrate_x");
            integrate_x(x, ty, y) = undef(t);
            integrate_x(-1, ty, y) = cast(t, 0);
            RDom rx_scan(0, width + diameter);
            integrate_x(rx_scan, ty, y) =
                (integrate_x(rx_scan - 1, ty, y) +
                 blur_y(rx_scan, ty, y));

            Func blur_x("blur_x");
            blur_x(x, ty, y) = integrate_x(x + diameter - 1, ty, y) - integrate_x(x - 1, ty, y);

            Func blur_y_untiled("blur_y_untiled");
            blur_y_untiled(x, y) = blur_y(x, y / N, y % N);

            // For small diameter, we do it directly and stay in 16-bit
            Func blur_x_direct("blur_x_direct");
            RDom rx_direct(0, diameter);
            blur_x_direct(x, y) += blur_y_untiled(x + rx_direct, y);

            auto norm = [&](Expr e) {
#if 1
                e = cast<float>(e);
                Expr den = cast<float>(diameter * diameter);
                Expr result = round(e * (1 / den));
                return cast<uint8_t>(result);
#elif 0
                if (e.type().bits() == 16) {
                    Expr den = cast<uint8_t>(diameter * diameter);
                    return cast<uint8_t>(fast_integer_divide(e + den / 2, den));
                } else {

                    e = cast<float>(e);
                    // If 23 bits is enough...
                    Expr inv_den = 1.0f / (256 * diameter * diameter);
                    // Get the result between [0 and 255/256], with 23 bits of precision
                    Expr result = e;
                    result *= inv_den;
                    // Map it to [1 + 1.0f/512, 2 - 1.0f/512]
                    result += 1.0f + 1.0f / 512;
                    // Extract the top 8 bits of the mantissa
                    return cast<uint8_t>(reinterpret<uint32_t>(result) >> 15);
                }
#else
                e = cast<double>(e);
                // 52 bits of precision, plus exact division
                Expr den = cast<double>(diameter * diameter);
                Expr result = round(e / den);
                return cast<uint8_t>(result);
#endif
            };

            Func normalize("normalize");
            normalize(x, y) = norm(blur_x(x, y / N, y % N));

            if (use_blur_x_direct) {
                results.push_back(norm(blur_x_direct(x, y)));
            } else {
                results.push_back(normalize(x, y));
            }
            conditions.push_back(diameter <= max_diameter);

            if (use_blur_x_direct) {
                blur_y
                    .compute_at(blur_y.in(), tx);
                blur_y.update(0)
                    .vectorize(x);
                blur_y.update(1)
                    .vectorize(x)
                    .unroll(ry_scan);

                blur_y.in()
                    .compute_at(output, yo)
                    .split(x, tx, x, vec)
                    .reorder(y, x, tx)
                    .vectorize(x)
                    .unroll(y);

            } else {

                normalize.compute_at(output, tx).reorder_storage(y, x).vectorize(y).unroll(x);
                normalize.in().compute_at(output, tx).vectorize(y).unroll(x);

                integrate_x.compute_at(output, yo)
                    .reorder_storage(y, x, ty);

                integrate_x.update(0).vectorize(y);
                integrate_x.update(1).vectorize(y);

                RVar rxo, rxi;
                integrate_x.update(1).split(rx_scan, rxo, rxi, vec).reorder(y, rxi, rxo, ty).unroll(rxi);

                // integrate_x.align_bounds(x, (diameter + width) * 2);

                /*
                blur_y.in()
                    .compute_at(integrate_x, rxo)
                    .reorder_storage(y, x, ty)
                    .reorder(y, x, ty)
                    .split(x, tx, x, 8)
                    .vectorize(x)
                    .unroll(y);
                */

                blur_y
                    .compute_at(integrate_x, rxo)
                    .store_in(MemoryType::Stack)
                    .bound_extent(x, vec);
                blur_y.update(0)
                    .vectorize(x);
                blur_y.update(1)
                    .vectorize(x)
                    .unroll(ry_scan);

                blur_y.in()
                    .compute_at(integrate_x, rxo)
                    .store_in(MemoryType::Stack)
                    .bound_extent(x, vec)
                    .reorder_storage(y, x, ty)
                    .vectorize(x)
                    .unroll(y);
            }

            blur_y_init
                .compute_at(output, ty)
                .align_bounds(x, vec)
                .vectorize(x, vec, TailStrategy::GuardWithIf);
            if (use_down_y) {
                blur_y_init.update(0).reorder(x, ry_init_fine_1, ty).vectorize(x, vec, TailStrategy::GuardWithIf);
                blur_y_init.update(1).reorder(x, ry_init_coarse, ty).vectorize(x, vec, TailStrategy::RoundUp);
                blur_y_init.update(2).reorder(x, ry_init_fine_2, ty).vectorize(x, vec, TailStrategy::GuardWithIf);
            } else {
                blur_y_init.update(0).unroll(ry_init_full, 2).reorder(x, ry_init_full, ty).vectorize(x, vec, TailStrategy::GuardWithIf);
            }

            /*
            blur_y_init
                .compute_at(integrate_x, rxo)
                .vectorize(x, vec, TailStrategy::GuardWithIf);
            if (use_down_y) {
                blur_y_init.update(0).reorder(x, ry_init_fine_1, ty).vectorize(x, vec, TailStrategy::GuardWithIf);
                blur_y_init.update(1).reorder(x, ry_init_coarse, ty).vectorize(x, vec, TailStrategy::RoundUp);
                blur_y_init.update(2).reorder(x, ry_init_fine_2, ty).vectorize(x, vec, TailStrategy::GuardWithIf);
            } else {
                blur_y_init.update(0).unroll(ry_init_full, 2).reorder(x, ry_init_full, ty).vectorize(x, vec, TailStrategy::GuardWithIf);
            }
            */
        }

        down_y.in().compute_root().parallel(y).vectorize(x, vec * 8).align_storage(x, vec);
        // down_y.compute_at(down_y.in(), yi).vectorize(x, vec).update().reorder(x, r_down, y).vectorize(x, vec);

        Expr result = 0;
        for (size_t i = conditions.size(); i > 0; i--) {
            result = select(conditions[i - 1], results[i - 1], result);
        }

        output(x, y) = result;

        output.align_bounds(y, N)
            .align_bounds(x, vec)
            .split(y, ty, y, N, TailStrategy::GuardWithIf)
            .split(y, yo, yi, 8)
            .split(x, tx, x, vec)
            .reorder(x, yi, tx, yo, ty)
            .parallel(ty)
            .vectorize(x)
            .unroll(yi);
        for (auto c : conditions) {
            output.specialize(c);
        }
        output.specialize_fail("Unsupported diameter");

        add_requirement(diameter > 0);
        add_requirement(diameter % 2 == 1);
    }
};

HALIDE_REGISTER_GENERATOR(BoxBlurPyramid, box_blur_pyramid)
