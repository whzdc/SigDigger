//
//    SymbolDifferentiatorFactory.h: Make symbol Differentiators
//    Copyright (C) 2019 Gonzalo José Carracedo Carballal
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Lesser General Public License as
//    published by the Free Software Foundation, either version 3 of the
//    License, or (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful, but
//    WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public
//    License along with this program.  If not, see
//    <http://www.gnu.org/licenses/>
//

#ifndef SYMBOLDIFFERENTIATORFACTORY_H
#define SYMBOLDIFFERENTIATORFACTORY_H

#include <Suscan/DecoderFactory.h>
#include <Decoder.h>

namespace SigDigger {
  class SymbolDifferentiatorFactory : public Suscan::DecoderFactory
  {
  public:
    std::string getName(void) const override;
    std::string getDescription(void) const override;
    Suscan::DecoderObjects *make(QWidget *parent = nullptr) override;
  };
}

#endif // SYMBOLDifferentiatorFACTORY_H
