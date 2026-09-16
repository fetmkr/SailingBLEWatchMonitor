#include "mag_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace magcal2 {
namespace {

constexpr double kPivotMin = 1e-10;
constexpr float kFieldMinUt = 25.0f;
constexpr float kFieldMaxUt = 70.0f;
constexpr float kCoverageMin = 0.30f;
constexpr float kConditionMax = 2.5f;
constexpr float kResidualMaxRatio = 0.08f;

bool finiteArray(const float* values, int count) {
    for (int i = 0; i < count; ++i) if (!std::isfinite(values[i])) return false;
    return true;
}

bool solve(double matrix[9][9], double rhs[9], double out[9]) {
    for (int column = 0; column < 9; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 9; ++row) {
            if (std::fabs(matrix[row][column]) > std::fabs(matrix[pivot][column])) pivot = row;
        }
        if (std::fabs(matrix[pivot][column]) < kPivotMin) return false;
        if (pivot != column) {
            for (int k = 0; k < 9; ++k) std::swap(matrix[column][k], matrix[pivot][k]);
            std::swap(rhs[column], rhs[pivot]);
        }
        for (int row = column + 1; row < 9; ++row) {
            const double factor = matrix[row][column] / matrix[column][column];
            for (int k = column; k < 9; ++k) matrix[row][k] -= factor * matrix[column][k];
            rhs[row] -= factor * rhs[column];
        }
    }
    for (int row = 8; row >= 0; --row) {
        double value = rhs[row];
        for (int k = row + 1; k < 9; ++k) value -= matrix[row][k] * out[k];
        out[row] = value / matrix[row][row];
    }
    return true;
}

bool solve3(const double matrix[3][3], const double rhs[3], double out[3]) {
    double augmented[3][4];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) augmented[row][column] = matrix[row][column];
        augmented[row][3] = rhs[row];
    }
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::fabs(augmented[row][column]) > std::fabs(augmented[pivot][column])) pivot = row;
        }
        if (std::fabs(augmented[pivot][column]) < kPivotMin) return false;
        if (pivot != column) for (int k = 0; k < 4; ++k) std::swap(augmented[column][k], augmented[pivot][k]);
        const double divisor = augmented[column][column];
        for (int k = column; k < 4; ++k) augmented[column][k] /= divisor;
        for (int row = 0; row < 3; ++row) {
            if (row == column) continue;
            const double factor = augmented[row][column];
            for (int k = column; k < 4; ++k) augmented[row][k] -= factor * augmented[column][k];
        }
    }
    for (int row = 0; row < 3; ++row) out[row] = augmented[row][3];
    return true;
}

// Jacobi decomposition for a real symmetric 3x3 matrix.
bool eigenSymmetric(const double input[3][3], double value[3], double vector[3][3]) {
    double a[3][3];
    std::memcpy(a, input, sizeof(a));
    std::memset(vector, 0, 9 * sizeof(double));
    for (int i = 0; i < 3; ++i) vector[i][i] = 1.0;
    for (int iteration = 0; iteration < 32; ++iteration) {
        int p = 0, q = 1;
        double largest = std::fabs(a[p][q]);
        if (std::fabs(a[0][2]) > largest) { p = 0; q = 2; largest = std::fabs(a[0][2]); }
        if (std::fabs(a[1][2]) > largest) { p = 1; q = 2; largest = std::fabs(a[1][2]); }
        if (largest < 1e-12) break;
        const double angle = 0.5 * std::atan2(2.0 * a[p][q], a[q][q] - a[p][p]);
        const double c = std::cos(angle), s = std::sin(angle);
        for (int k = 0; k < 3; ++k) {
            if (k == p || k == q) continue;
            const double akp = a[k][p], akq = a[k][q];
            a[k][p] = a[p][k] = c * akp - s * akq;
            a[k][q] = a[q][k] = s * akp + c * akq;
        }
        const double app = a[p][p], aqq = a[q][q], apq = a[p][q];
        a[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
        a[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
        a[p][q] = a[q][p] = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double vkp = vector[k][p], vkq = vector[k][q];
            vector[k][p] = c * vkp - s * vkq;
            vector[k][q] = s * vkp + c * vkq;
        }
    }
    for (int i = 0; i < 3; ++i) value[i] = a[i][i];
    for (int i = 0; i < 2; ++i) for (int j = i + 1; j < 3; ++j) {
        if (value[j] < value[i]) {
            std::swap(value[i], value[j]);
            for (int row = 0; row < 3; ++row) std::swap(vector[row][i], vector[row][j]);
        }
    }
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

void symmetricFromEigen(const double vector[3][3], const double diagonal[3], double out[3][3]) {
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        out[row][column] = 0.0;
        for (int k = 0; k < 3; ++k) out[row][column] += vector[row][k] * diagonal[k] * vector[column][k];
    }
}

float cloudCoverage(const int16_t (*points)[3], int count, const double mean[3], double scale) {
    double covariance[3][3] = {};
    for (int i = 0; i < count; ++i) {
        double d[3];
        for (int axis = 0; axis < 3; ++axis) d[axis] = (points[i][axis] * 0.1 - mean[axis]) / scale;
        for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
            covariance[row][column] += d[row] * d[column];
        }
    }
    for (auto& row : covariance) for (double& value : row) value /= count;
    double eigenvalue[3], eigenvector[3][3];
    if (!eigenSymmetric(covariance, eigenvalue, eigenvector) || eigenvalue[2] <= 0.0 || eigenvalue[0] < 0.0) return 0.0f;
    return static_cast<float>(std::sqrt(eigenvalue[0] / eigenvalue[2]));
}

}  // namespace

Calibration identity() { return {}; }

void apply(const Calibration& calibration, const float raw[3], float corrected[3]) {
    const float centered[3] = {raw[0] - calibration.bias[0], raw[1] - calibration.bias[1], raw[2] - calibration.bias[2]};
    for (int row = 0; row < 3; ++row) {
        corrected[row] = calibration.matrix[row * 3] * centered[0] +
                         calibration.matrix[row * 3 + 1] * centered[1] +
                         calibration.matrix[row * 3 + 2] * centered[2];
    }
}

bool usable(const Calibration& calibration) {
    if (calibration.version > 2 || !finiteArray(calibration.bias, 3) || !finiteArray(calibration.matrix, 9) ||
        !std::isfinite(calibration.fieldUt) || !std::isfinite(calibration.rmsUt) ||
        !std::isfinite(calibration.condition) || !std::isfinite(calibration.coverage)) return false;
    double matrix[3][3];
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        matrix[row][column] = calibration.matrix[row * 3 + column];
        if (std::fabs(matrix[row][column] - calibration.matrix[column * 3 + row]) > 1e-3) return false;
    }
    double eigenvalue[3], eigenvector[3][3];
    if (!eigenSymmetric(matrix, eigenvalue, eigenvector) || eigenvalue[0] < 0.2 || eigenvalue[2] > 5.0) return false;
    const double matrixCondition = eigenvalue[2] / eigenvalue[0];
    if (!std::isfinite(matrixCondition) || matrixCondition > kConditionMax) return false;
    if (calibration.condition < 1.0f || calibration.condition > kConditionMax) return false;
    if (calibration.version == 2) {
        if (calibration.fieldUt < kFieldMinUt || calibration.fieldUt > kFieldMaxUt) return false;
        if (calibration.coverage < kCoverageMin) return false;
        if (calibration.rmsUt < 0.0f || calibration.rmsUt > calibration.fieldUt * kResidualMaxRatio) return false;
    }
    return true;
}

Verdict fitAndJudge(const int16_t (*points)[3], int count, Calibration* result) {
    Calibration calibration;
    if (result) *result = calibration;
    if (!points || count < kMinPoints) return Verdict::TooFew;

    double mean[3] = {};
    for (int i = 0; i < count; ++i) for (int axis = 0; axis < 3; ++axis) mean[axis] += points[i][axis] * 0.1;
    for (double& value : mean) value /= count;
    double squareSum = 0.0;
    for (int i = 0; i < count; ++i) for (int axis = 0; axis < 3; ++axis) {
        const double d = points[i][axis] * 0.1 - mean[axis];
        squareSum += d * d;
    }
    const double scale = std::sqrt(squareSum / count);
    if (!std::isfinite(scale) || scale < 1.0) return Verdict::Singular;
    calibration.coverage = cloudCoverage(points, count, mean, scale);
    if (calibration.coverage < kCoverageMin) {
        if (result) *result = calibration;
        return Verdict::PoorCoverage;
    }

    double normal[9][9] = {};
    double rhs[9] = {};
    for (int i = 0; i < count; ++i) {
        const double x = (points[i][0] * 0.1 - mean[0]) / scale;
        const double y = (points[i][1] * 0.1 - mean[1]) / scale;
        const double z = (points[i][2] * 0.1 - mean[2]) / scale;
        const double row[9] = {x*x, y*y, z*z, 2*x*y, 2*x*z, 2*y*z, 2*x, 2*y, 2*z};
        for (int r = 0; r < 9; ++r) {
            rhs[r] += row[r];
            for (int c = 0; c < 9; ++c) normal[r][c] += row[r] * row[c];
        }
    }
    double parameter[9] = {};
    if (!solve(normal, rhs, parameter)) return Verdict::Singular;
    const double a[3][3] = {{parameter[0], parameter[3], parameter[4]},
                            {parameter[3], parameter[1], parameter[5]},
                            {parameter[4], parameter[5], parameter[2]}};
    const double d[3] = {parameter[6], parameter[7], parameter[8]};
    const double negativeD[3] = {-d[0], -d[1], -d[2]};
    double center[3];
    if (!solve3(a, negativeD, center)) return Verdict::Singular;
    double centerAc = 0.0;
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        centerAc += center[row] * a[row][column] * center[column];
    }
    const double k = 1.0 + centerAc;
    if (!std::isfinite(k) || k <= 0.0) return Verdict::NotEllipsoid;
    double q[3][3];
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) q[row][column] = a[row][column] / k;
    double eigenvalue[3], eigenvector[3][3];
    if (!eigenSymmetric(q, eigenvalue, eigenvector) || eigenvalue[0] <= 0.0) return Verdict::NotEllipsoid;
    calibration.condition = static_cast<float>(std::sqrt(eigenvalue[2] / eigenvalue[0]));
    if (calibration.condition > kConditionMax) {
        if (result) *result = calibration;
        return Verdict::DistortionHigh;
    }
    calibration.fieldUt = static_cast<float>(scale / std::pow(eigenvalue[0] * eigenvalue[1] * eigenvalue[2], 1.0 / 6.0));
    if (calibration.fieldUt < kFieldMinUt || calibration.fieldUt > kFieldMaxUt) {
        if (result) *result = calibration;
        return Verdict::FieldOut;
    }
    for (int axis = 0; axis < 3; ++axis) calibration.bias[axis] = static_cast<float>(mean[axis] + scale * center[axis]);
    const double root[3] = {std::sqrt(eigenvalue[0]), std::sqrt(eigenvalue[1]), std::sqrt(eigenvalue[2])};
    double squareRoot[3][3];
    symmetricFromEigen(eigenvector, root, squareRoot);
    const double factor = calibration.fieldUt / scale;
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        calibration.matrix[row * 3 + column] = static_cast<float>(squareRoot[row][column] * factor);
    }
    calibration.version = 2;

    double residualSum = 0.0;
    for (int i = 0; i < count; ++i) {
        const float raw[3] = {points[i][0] * 0.1f, points[i][1] * 0.1f, points[i][2] * 0.1f};
        float corrected[3];
        apply(calibration, raw, corrected);
        const double norm = std::sqrt(corrected[0] * corrected[0] + corrected[1] * corrected[1] + corrected[2] * corrected[2]);
        const double residual = norm - calibration.fieldUt;
        residualSum += residual * residual;
    }
    calibration.rmsUt = static_cast<float>(std::sqrt(residualSum / count));
    if (result) *result = calibration;
    if (!usable(calibration)) return Verdict::NotEllipsoid;
    if (calibration.rmsUt > calibration.fieldUt * kResidualMaxRatio) return Verdict::ResidualHigh;
    return Verdict::Ok;
}

const char* verdictText(Verdict verdict) {
    switch (verdict) {
        case Verdict::Ok: return "통과";
        case Verdict::TooFew: return "점이 모자람 (80개 넘게)";
        case Verdict::Singular: return "풀리지 않음 (같은 방향만 측정)";
        case Verdict::NotEllipsoid: return "올바른 타원체가 아님";
        case Verdict::FieldOut: return "자기장 크기가 지구 자기장 범위 밖";
        case Verdict::PoorCoverage: return "방향이 한쪽에 몰림";
        case Verdict::DistortionHigh: return "보정하기엔 자력 왜곡이 너무 큼";
        case Verdict::ResidualHigh: return "보정 뒤에도 자기장 크기가 일정하지 않음";
    }
    return "?";
}

}  // namespace magcal2
